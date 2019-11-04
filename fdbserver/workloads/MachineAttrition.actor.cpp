/*
 * MachineAttrition.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/CoordinationInterface.h"
#include "fdbserver/ClusterRecruitmentInterface.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbrpc/simulator.h"
#include "fdbclient/ManagementAPI.actor.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

static std::set<int> const& normalAttritionErrors() {
	static std::set<int> s;
	if (s.empty()) {
		s.insert( error_code_please_reboot );
		s.insert( error_code_please_reboot_delete );
	}
	return s;
}

ACTOR Future<bool> ignoreSSFailuresForDuration(Database cx, double duration) {
	// duration doesn't matter since this won't timeout
	TraceEvent("IgnoreSSFailureStart");
	wait(success(setHealthyZone(cx, ignoreSSFailuresZoneString, 0)));
	TraceEvent("IgnoreSSFailureWait");
	wait(delay(duration));
	TraceEvent("IgnoreSSFailureClear");
	state Transaction tr(cx);
	loop {
		try {
			tr.setOption(FDBTransactionOptions::LOCK_AWARE);
			tr.clear(healthyZoneKey);
			wait(tr.commit());
			TraceEvent("IgnoreSSFailureComplete");
			return true;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

struct MachineAttritionWorkload : TestWorkload {
	bool enabled;
	int machinesToKill, machinesToLeave;
	double testDuration, suspendDuration;
	bool reboot;
	bool killDc;
	bool killMachine;
	bool killDatahall;
	bool killProcess;
	bool killSelf;
	Standalone<StringRef> targetId;
	bool replacement;
	bool waitForVersion;
	bool allowFaultInjection;
	Future<bool> ignoreSSFailures;

	// This is set in setup from the list of workers when the cluster is started
	std::vector<LocalityData> machines;

	MachineAttritionWorkload( WorkloadContext const& wcx )
		: TestWorkload(wcx)
	{
		enabled = !clientId && g_network->isSimulated(); // only do this on the "first" client, and only when in simulation
		machinesToKill = getOption( options, LiteralStringRef("machinesToKill"), 2 );
		machinesToLeave = getOption( options, LiteralStringRef("machinesToLeave"), 1 );
		testDuration = getOption( options, LiteralStringRef("testDuration"), 10.0 );
		suspendDuration = getOption( options, LiteralStringRef("suspendDuration"), 1.0 );
		reboot = getOption( options, LiteralStringRef("reboot"), false );
		killDc = getOption( options, LiteralStringRef("killDc"), deterministicRandom()->random01() < 0.25 );
		killMachine = getOption( options, LiteralStringRef("killMachine"), false);
		killDatahall = getOption( options, LiteralStringRef("killDatahall"), false);
		killProcess = getOption( options, LiteralStringRef("killProcess"), false);
		killSelf = getOption( options, LiteralStringRef("killSelf"), false );
		targetId = getOption( options, LiteralStringRef("targetId"), LiteralStringRef(""));
		replacement = getOption( options, LiteralStringRef("replacement"), reboot && deterministicRandom()->random01() < 0.5 );
		waitForVersion = getOption( options, LiteralStringRef("waitForVersion"), false );
		allowFaultInjection = getOption( options, LiteralStringRef("allowFaultInjection"), true );
		ignoreSSFailures = true;
	}

	static vector<ISimulator::ProcessInfo*> getServers() {
		vector<ISimulator::ProcessInfo*> machines;
		vector<ISimulator::ProcessInfo*> all = g_simulator.getAllProcesses();
		for(int i = 0; i < all.size(); i++)
			if( !all[i]->failed && all[i]->name == std::string("Server") && all[i]->startingClass != ProcessClass::TesterClass)
				machines.push_back( all[i] );
		return machines;
	}

	virtual std::string description() { return "MachineAttritionWorkload"; }
	virtual Future<Void> setup( Database const& cx ) {
		return Void();
	}
	virtual Future<Void> start( Database const& cx ) {
		if (enabled) {
			std::map<Optional<Standalone<StringRef>>,LocalityData> machineIDMap;
			auto processes = getServers();
			for (auto it = processes.begin(); it != processes.end(); ++it) {
				machineIDMap[(*it)->locality.zoneId()] = (*it)->locality;
			}
			machines.clear();
			for (auto it = machineIDMap.begin(); it != machineIDMap.end(); ++it) {
				machines.push_back(it->second);
			}
			deterministicRandom()->randomShuffle( machines );
			double meanDelay = testDuration / machinesToKill;
			TraceEvent("AttritionStarting")
				.detail("KillDataCenters", killDc)
				.detail("Reboot", reboot)
				.detail("MachinesToLeave", machinesToLeave)
				.detail("MachinesToKill", machinesToKill)
				.detail("MeanDelay", meanDelay);

			return timeout(
				reportErrorsExcept( machineKillWorker( this, meanDelay, cx ), "machineKillWorkerError", UID(), &normalAttritionErrors()),
				testDuration, Void() );
		}
		if (!clientId && !g_network->isSimulated()) {
			return timeout(
				reportErrorsExcept(noSimMachineKillWorker(this, cx), "noSimMachineKillWorkerError", UID(), &normalAttritionErrors()),
			    testDuration, Void());
		}
		if(killSelf)
			throw please_reboot();
		return Void();
	}
	virtual Future<bool> check( Database const& cx ) { return ignoreSSFailures; }
	virtual void getMetrics( vector<PerfMetric>& m ) {
	}

	static bool noSimIsViableKill(WorkerDetails worker) {
		return (worker.processClass == ProcessClass::ClassType::TesterClass);
	}

	ACTOR static Future<Void> noSimMachineKillWorker(MachineAttritionWorkload *self, Database cx) {
		ASSERT(!g_network->isSimulated());
		state int killedMachines = 0;
		state std::vector<WorkerDetails> allWorkers =
		    wait(self->dbInfo->get().clusterInterface.getWorkers.getReply(GetWorkersRequest()));
		// Can reuse reboot request to send to each interface since no reply promise needed
		state RebootRequest rbReq;
		if (self->reboot) {
			rbReq.waitForDuration = self->suspendDuration;
		} else {
			rbReq.waitForDuration = std::numeric_limits<uint32_t>::max();
		}
		state std::vector<WorkerDetails> workers;
		// Pre-processing step: remove all testers from list of workers
		for (const auto& worker : allWorkers) {
			if (noSimIsViableKill(worker)) {
				workers.push_back(worker);
			}
		}
		deterministicRandom()->randomShuffle(workers);
		if (self->killDc) {
			// Pick a dcId to kill
			Optional<Standalone<StringRef>> killDcId = self->targetId.toString().empty() ? workers.back().interf.locality.dcId() : self->targetId;
			TraceEvent("Assassination").detail("TargetDataCenterId", killDcId);
			for (const auto& worker : workers) {
				// kill all matching dcId workers
				if (worker.interf.locality.dcId().present() && worker.interf.locality.dcId() == killDcId) {
					TraceEvent("SendingRebootRequest").detail("TargetMachine", worker.interf.locality.toString());
					worker.interf.clientInterface.reboot.send(rbReq);
				}
			}
		} else if (self->killMachine) {
			// Pick a machine to kill
			Optional<Standalone<StringRef>> killMachineId = self->targetId.toString().empty() ? workers.back().interf.locality.machineId() : self->targetId;
			TraceEvent("Assassination").detail("TargetMachineId", killMachineId);
			for (const auto& worker : workers) {
				// kill all matching machine workers
				if (worker.interf.locality.machineId().present() && worker.interf.locality.machineId() == killMachineId) {
					TraceEvent("SendingRebootRequest").detail("TargetMachine", worker.interf.locality.toString());
					worker.interf.clientInterface.reboot.send(rbReq);
				}
			}
		} else if (self->killDatahall) {
			// Pick a datahall to kill
			Optional<Standalone<StringRef>> killDatahallId = self->targetId.toString().empty() ? workers.back().interf.locality.dataHallId() : self->targetId;
			TraceEvent("Assassination").detail("TargetDatahallId", killDatahallId);
			for (const auto& worker : workers) {
				// kill all matching datahall workers
				if (worker.interf.locality.dataHallId().present() && worker.interf.locality.dataHallId() == killDatahallId) {
					TraceEvent("SendingRebootRequest").detail("TargetMachine", worker.interf.locality.toString());
					worker.interf.clientInterface.reboot.send(rbReq);
				}
			}
		} else if (self->killProcess) {
			// Pick a process to kill
			Optional<Standalone<StringRef>> killProcessId = self->targetId.toString().empty() ? workers.back().interf.locality.processId() : self->targetId;
			TraceEvent("Assassination").detail("TargetProcessId", killProcessId);
			for (const auto& worker : workers) {
				// kill matching processes
				if (worker.interf.locality.processId().present() && worker.interf.locality.processId() == killProcessId) {
					TraceEvent("SendingRebootRequest").detail("TargetMachine", worker.interf.locality.toString());
					worker.interf.clientInterface.reboot.send(rbReq);
				}
			}
		} else {
			while (killedMachines < self->machinesToKill && workers.size() > self->machinesToLeave) {
				TraceEvent("WorkerKillBegin")
				    .detail("KilledMachines", killedMachines)
				    .detail("MachinesToKill", self->machinesToKill)
				    .detail("MachinesToLeave", self->machinesToLeave)
				    .detail("Machines", workers.size());
				if (self->waitForVersion) {
					state Transaction tr(cx);
					loop {
						try {
							tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
							tr.setOption(FDBTransactionOptions::LOCK_AWARE);
							wait(success(tr.getReadVersion()));
							break;
						} catch (Error& e) {
							wait(tr.onError(e));
						}
					}
				}
				// Pick a machine to kill
				state WorkerDetails targetMachine;
				targetMachine = workers.back();
				TraceEvent("Assassination")
				    .detail("TargetMachine", targetMachine.interf.locality.toString())
				    .detail("ZoneId", targetMachine.interf.locality.zoneId())
				    .detail("KilledMachines", killedMachines)
				    .detail("MachinesToKill", self->machinesToKill)
				    .detail("MachinesToLeave", self->machinesToLeave)
				    .detail("Machines", workers.size());
				targetMachine.interf.clientInterface.reboot.send(rbReq);
				killedMachines++;
				workers.pop_back();
			}
		}
		return Void();
	}

	ACTOR static Future<Void> machineKillWorker( MachineAttritionWorkload *self, double meanDelay, Database cx ) {
		state int killedMachines = 0;
		state double delayBeforeKill = deterministicRandom()->random01() * meanDelay;

		ASSERT( g_network->isSimulated() );

		if( self->killDc ) {
			wait( delay( delayBeforeKill ) );

			// decide on a machine to kill
			ASSERT( self->machines.size() );
			Optional<Standalone<StringRef>> target = self->machines.back().dcId();

			ISimulator::KillType kt = ISimulator::Reboot;
			if( !self->reboot ) {
				int killType = deterministicRandom()->randomInt(0,3);
				if( killType == 0 )
					kt = ISimulator::KillInstantly;
				else if( killType == 1 )
					kt = ISimulator::InjectFaults;
				else
					kt = ISimulator::RebootAndDelete;
			}
			TraceEvent("Assassination").detail("TargetDatacenter", target).detail("Reboot", self->reboot).detail("KillType", kt);

			g_simulator.killDataCenter( target, kt );
		} else {
			while ( killedMachines < self->machinesToKill && self->machines.size() > self->machinesToLeave) {
				TraceEvent("WorkerKillBegin").detail("KilledMachines", killedMachines)
					.detail("MachinesToKill", self->machinesToKill).detail("MachinesToLeave", self->machinesToLeave)
					.detail("Machines", self->machines.size());
				TEST(true);  // Killing a machine

				wait( delay( delayBeforeKill ) );
				TraceEvent("WorkerKillAfterDelay");

				if(self->waitForVersion) {
					state Transaction tr( cx );
					loop {
						try {
							tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
							tr.setOption(FDBTransactionOptions::LOCK_AWARE);
							wait(success(tr.getReadVersion()));
							break;
						} catch( Error &e ) {
							wait( tr.onError(e) );
						}
					}
				}

				// decide on a machine to kill
				state LocalityData targetMachine = self->machines.back();
				if(BUGGIFY_WITH_PROB(0.01)) {
					TEST(true); //Marked a zone for maintenance before killing it
					wait(success(
					    setHealthyZone(cx, targetMachine.zoneId().get(), deterministicRandom()->random01() * 20)));
				} else if (BUGGIFY_WITH_PROB(0.005)) {
					TEST(true); // Disable DD for all storage server failures
					self->ignoreSSFailures =
					    uncancellable(ignoreSSFailuresForDuration(cx, deterministicRandom()->random01() * 5));
				}

				TraceEvent("Assassination").detail("TargetMachine", targetMachine.toString())
					.detail("ZoneId", targetMachine.zoneId())
					.detail("Reboot", self->reboot).detail("KilledMachines", killedMachines)
					.detail("MachinesToKill", self->machinesToKill).detail("MachinesToLeave", self->machinesToLeave)
					.detail("Machines", self->machines.size()).detail("Replace", self->replacement);

				if (self->reboot) {
					if( deterministicRandom()->random01() > 0.5 ) {
						g_simulator.rebootProcess( targetMachine.zoneId(), deterministicRandom()->random01() > 0.5 );
					} else {
						g_simulator.killZone( targetMachine.zoneId(), ISimulator::Reboot );
					}
				} else {
					auto randomDouble = deterministicRandom()->random01();
					TraceEvent("WorkerKill").detail("MachineCount", self->machines.size()).detail("RandomValue", randomDouble);
					if (randomDouble < 0.33 ) {
						TraceEvent("RebootAndDelete").detail("TargetMachine", targetMachine.toString());
						g_simulator.killZone( targetMachine.zoneId(), ISimulator::RebootAndDelete );
					} else {
						auto kt = (deterministicRandom()->random01() < 0.5 || !self->allowFaultInjection) ? ISimulator::KillInstantly : ISimulator::InjectFaults;
						g_simulator.killZone( targetMachine.zoneId(), kt );
					}
				}

				killedMachines++;
				if(!self->replacement)
					self->machines.pop_back();

				wait(delay(meanDelay - delayBeforeKill) && success(self->ignoreSSFailures));

				delayBeforeKill = deterministicRandom()->random01() * meanDelay;
				TraceEvent("WorkerKillAfterMeanDelay").detail("DelayBeforeKill", delayBeforeKill);
			}
		}

		if(self->killSelf)
			throw please_reboot();
		return Void();
	}
};

WorkloadFactory<MachineAttritionWorkload> MachineAttritionWorkloadFactory("Attrition");

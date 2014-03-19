#include "obd2.h"
#include "can/canutil.h"
#include "util/timer.h"
#include "util/log.h"
#include "shared_handlers.h"
#include "config.h"
#include <limits.h>

namespace time = openxc::util::time;

using openxc::diagnostics::DiagnosticsManager;
using openxc::diagnostics::obd2::Obd2Pid;
using openxc::util::log::debug;
using openxc::diagnostics::ActiveDiagnosticRequest;
using openxc::config::getConfiguration;
using openxc::config::PowerManagement;

#define ENGINE_SPEED_PID 0xc
#define VEHICLE_SPEED_PID 0xd

static bool ENGINE_STARTED = false;
static bool VEHICLE_IN_MOTION = false;

static openxc::util::time::FrequencyClock IGNITION_STATUS_TIMER = {0.2};

const Obd2Pid OBD2_PIDS[] = {
    { pid: ENGINE_SPEED_PID, name: "engine_speed", frequency: 5 },
    { pid: VEHICLE_SPEED_PID, name: "vehicle_speed", frequency: 5 },
    { pid: 0x4, name: "engine_load", frequency: 5 },
    { pid: 0x33, name: "barometric_pressure", frequency: 1 },
    { pid: 0x4c, name: "commanded_throttle_position", frequency: 1 },
    { pid: 0x5, name: "engine_coolant_temperature", frequency: 1 },
    { pid: 0x27, name: "fuel_level", frequency: 1 },
    { pid: 0xf, name: "intake_air_temperature", frequency: 1 },
    { pid: 0xb, name: "intake_manifold_pressure", frequency: 1 },
    { pid: 0x1f, name: "running_time", frequency: 1 },
    { pid: 0x11, name: "throttle_position", frequency: 5 },
    { pid: 0xa, name: "fuel_pressure", frequency: 1 },
    { pid: 0x66, name: "mass_airflow", frequency: 5 },
    { pid: 0x5a, name: "accelerator_pedal_position", frequency: 5 },
    { pid: 0x52, name: "ethanol_fuel_percentage", frequency: 1 },
    { pid: 0x5c, name: "engine_oil_temperature", frequency: 1 },
    { pid: 0x63, name: "engine_torque", frequency: 1 },
};

static void checkIgnitionStatus(DiagnosticsManager* manager,
        const ActiveDiagnosticRequest* request,
        const DiagnosticResponse* response,
        float parsedPayload) {
    float value = diagnostic_decode_obd2_pid(response);
    bool match = false;
    if(response->pid == ENGINE_SPEED_PID) {
        match = ENGINE_STARTED = value != 0;
    } else if(response->pid == VEHICLE_SPEED_PID) {
        match = VEHICLE_IN_MOTION = value != 0;
    }

    if(match) {
        time::tick(&IGNITION_STATUS_TIMER);
    }
}

static void requestIgnitionStatus(DiagnosticsManager* manager) {
    if(manager->obd2Bus != NULL && (getConfiguration()->powerManagement ==
                PowerManagement::OBD2_IGNITION_CHECK ||
            getConfiguration()->recurringObd2Requests)) {
        DiagnosticRequest request = {arbitration_id: OBD2_FUNCTIONAL_BROADCAST_ID,
                mode: 0x1, has_pid: true, pid: ENGINE_SPEED_PID};
        addRequest(manager, manager->obd2Bus, &request, "engine_speed",
                false, false, 1, 0, NULL, checkIgnitionStatus);

        request.pid = VEHICLE_SPEED_PID;
        addRequest(manager, manager->obd2Bus, &request, "vehicle_speed",
                false, false, 1, 0, NULL, checkIgnitionStatus);
        time::tick(&IGNITION_STATUS_TIMER);
    }
}

static void checkSupportedPids(DiagnosticsManager* manager,
        const ActiveDiagnosticRequest* request,
        const DiagnosticResponse* response,
        float parsedPayload) {
    if(manager->obd2Bus == NULL || !getConfiguration()->recurringObd2Requests) {
        return;
    }

    for(int i = 0; i < response->payload_length; i++) {
        for(int j = CHAR_BIT - 1; j >= 0; j--) {
            if(response->payload[i] >> j & 0x1) {
                uint16_t pid = response->pid + (i * CHAR_BIT) + j + 1;
                DiagnosticRequest request = {
                        arbitration_id: OBD2_FUNCTIONAL_BROADCAST_ID,
                        mode: 0x1, has_pid: true, pid: pid};
                for(size_t i = 0; i < sizeof(OBD2_PIDS) / sizeof(Obd2Pid); i++) {
                    if(OBD2_PIDS[i].pid == pid) {
                        debug("Vehicle supports PID %d", pid);
                        addRecurringRequest(manager, manager->obd2Bus, &request,
                                OBD2_PIDS[i].name, false, false, 1, 0,
                                openxc::signals::handlers::handleObd2Pid,
                                checkIgnitionStatus,
                                OBD2_PIDS[i].frequency);
                        break;
                    }
                }
            }
        }
    }
}

void openxc::diagnostics::obd2::initialize(DiagnosticsManager* manager) {
    requestIgnitionStatus(manager);
}

// * CAN traffic will eventualy stop, and we will suspend.
// * When do we wake up?
// * If normal CAN is open, bus activity will wake us up and we will resume.
// * If normal CAN is blocked, we rely on a watchdog to wake us up every 15
// seconds to start this process over again.
void openxc::diagnostics::obd2::loop(DiagnosticsManager* manager, CanBus* bus) {
    static bool ignitionWasOn = false;
    static bool pidSupportQueried = false;
    static bool sentFinalIgnitionCheck = false;

    if(!manager->initialized) {
        return;
    }

    if(time::elapsed(&IGNITION_STATUS_TIMER, false)) {
        if(sentFinalIgnitionCheck) {
            // remove all open diagnostic requests, which shuld cause the bus to go
            // silent if the car is off, and thus the VI to suspend. TODO kick off
            // watchdog! TODO when it wakes keep in a minimum run level (i.e. don't
            // turn on bluetooth) until we decide the vehicle is actually on.
            if(manager->initialized && getConfiguration()->powerManagement ==
                        PowerManagement::OBD2_IGNITION_CHECK) {
                debug("Ceasing diagnostic requests as ignition went off");
                diagnostics::reset(manager);
                manager->initialized = false;
            }
            ignitionWasOn = false;
            pidSupportQueried = false;
        } else {
            // We haven't received an ignition in 5 seconds. Either the user didn't
            // have either OBD-II request configured as a recurring request (which
            // is fine) or they did, but the car stopped responding. Kick off
            // another request to see which is true. It will take 5+5 seconds after
            // ignition off to decide we should shut down.
            requestIgnitionStatus(manager);
            sentFinalIgnitionCheck = true;
        }
    } else if(!ignitionWasOn && (ENGINE_STARTED || VEHICLE_IN_MOTION)) {
        ignitionWasOn = true;
        sentFinalIgnitionCheck = false;
        if(getConfiguration()->recurringObd2Requests && !pidSupportQueried) {
            debug("Ignition is on - querying for supported OBD-II PIDs");
            pidSupportQueried = true;
            DiagnosticRequest request = {
                    arbitration_id: OBD2_FUNCTIONAL_BROADCAST_ID,
                    mode: 0x1,
                    has_pid: true,
                    pid: 0x0};
            for(int i = 0x0; i <= 0x80; i += 0x20) {
                request.pid = i;
                addRequest(manager, bus, &request, NULL, false, false, 1, 0,
                        NULL, checkSupportedPids);
            }
        }
    }
}

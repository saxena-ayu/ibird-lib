/*
 * Copyright (c) 2009 - 2012, Regents of the University of California
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the University of California, Berkeley nor the names
 *   of its contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * I-Bird Attitude Regulation Module
 *
 * by Stanley S. Baek
 *
 * v.beta
 *
 * Revisions:
 *  Stanley S. Baek     2009-10-30      Initial release
 *  Humphrey Hu		    2011-07-20       Changed to fixed point
 *  Humphrey Hu         2012-02-20       Returned to floating point, restructured
 *  Humphrey Hu         2012-06-30      Switched to using quaternions
 *
 * Notes:
 *  I-Bird body axes are: (check these)
 *      x - Forward along anteroposterior axis
 *      y - Left along left-right axis
 *      z - Up along dorsoventral axis
 *  Rotations in body axes are:
 *      yaw - Positive z direction
 *      pitch - Positive y direction
 *      roll - Positive x direction
 */

// Software modules
#include "regulator.h"
#include "controller.h"
#include "dfilter.h"
#include "attitude.h"
#include "cv.h"

// Hardware/actuator interface
#include "motor_ctrl.h"
#include "sync_servo.h"
#include "led.h"

// Other
#include "quat.h"
#include "sys_clock.h"
#include "bams.h"
#include "utils.h"
#include "pbuff.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    float thrust;
    float steer;
    float elevator;
} RegulatorOutput;

#define REG_BUFF_SIZE       (5)

#define YAW_SAT_MAX         (1.0)
#define YAW_SAT_MIN         (-1.0)
#define PITCH_SAT_MAX       (1.0)
#define PITCH_SAT_MIN       (-1.0)
#define ROLL_SAT_MAX        (1.0)
#define ROLL_SAT_MIN        (-1.0)

// =========== Static Variables ================================================

// Lower level components
CtrlPidParamStruct yawPid, pitchPid, rollPid;
DigitalFilterStruct yawRateFilter, pitchRateFilter, rollRateFilter;

// State info
static unsigned char is_ready = 0;
static RegulatorMode reg_mode;
static RegulatorOutput rc_outputs;
static Quaternion reference;
// Telemetry buffering
static RegulatorStateStruct reg_state[REG_BUFF_SIZE];
static PoolBuffStruct reg_state_buff;

// =========== Function Stubs =================================================
static float runYawControl(float yaw);
static float runPitchControl(float pitch);
static float runRollControl(float roll);

// =========== Public Functions ===============================================

void rgltrSetup(float ts) {

    unsigned int i;
    float xcoeffs[4] = {1.0/6.0, 3.0, 3.0, 1.0};
    float ycoeffs[4] = {0.0, 0.0, 1.0/3.0, 0.0};
    RegulatorState states[REG_BUFF_SIZE];

    RateFilterParamsStruct f_params;
    
    is_ready = 0;
    reg_mode = REG_OFF;

    servoSetup();
    mcSetup();  // Set up motor driver

    ctrlInitPidParams(&yawPid, ts);
    ctrlInitPidParams(&pitchPid, ts);
    ctrlInitPidParams(&rollPid, ts);    

    for(i = 0; i < REG_BUFF_SIZE; i++) {
        states[i] = &reg_state[i];
    }
    pbuffInit(&reg_state_buff, REG_BUFF_SIZE, states);
    
    reference.w = 1.0;
    reference.x = 0.0;
    reference.y = 0.0;
    reference.z = 0.0;
    
    f_params.order = 3;
    f_params.type = 0;
    f_params.xcoeffs = xcoeffs;
    f_params.ycoeffs = ycoeffs;
    rgltrSetYawRateFilter(&f_params);
    
    is_ready = 1;
}

void rgltrSetMode(unsigned char flag) {

    if(flag == REG_OFF) {
        reg_mode = flag;
        ctrlStop(&yawPid);
        ctrlStop(&pitchPid);
        ctrlStop(&rollPid);
        servoStop();
    } else if(flag == REG_TRACK) {
        reg_mode = flag;
        ctrlStart(&yawPid);
        ctrlStart(&pitchPid);
        ctrlStart(&rollPid);
        servoStart();
    } else if(flag == REG_REMOTE_CONTROL) {
        reg_mode = flag;
        ctrlStop(&yawPid);
        ctrlStop(&pitchPid);
        ctrlStop(&rollPid);
        servoStart();
    }
        
}

void rgltrSetYawRateFilter(RateFilterParams params) {

    yawRateFilter = dfilterCreate(params->order, params->type,
                                    // params->xcoeffs, params->ycoeffs);
    dfilterInit(params->order, params->type, params->xcoeffs, params->ycoeffs);
    
} 


void rgltrSetPitchRateFilter(RateFilterParams params) {

    pitchRateFilter = dfilterCreate(params->order, params->type,
                                    params->xcoeffs, params->ycoeffs);

} 

void rgltrSetRollRateFilter(RateFilterParams params) {

    rollRateFilter = dfilterCreate(params->order, params->type,
                                    params->xcoeffs, params->ycoeffs);

}

void rgltrSetYawPid(PidParams params) {
    
    ctrlSetPidParams(&yawPid, params->ref, params->kp, params->ki, params->kd);
    ctrlSetPidOffset(&yawPid, params->offset);
    ctrlSetRefWeigts(&yawPid, params->beta, params->gamma);
    ctrlSetSaturation(&yawPid, YAW_SAT_MAX, YAW_SAT_MIN);

}

void rgltrSetPitchPid(PidParams params) {
    
    ctrlSetPidParams(&pitchPid, params->ref, params->kp, params->ki, params->kd);
    ctrlSetPidOffset(&pitchPid, params->offset);
    ctrlSetRefWeigts(&pitchPid, params->beta, params->gamma);
    ctrlSetSaturation(&pitchPid, PITCH_SAT_MAX, PITCH_SAT_MIN);

}

void rgltrSetRollPid(PidParams params) {

    ctrlSetPidParams(&rollPid, params->ref, params->kp, params->ki, params->kd);
    ctrlSetPidOffset(&rollPid, params->offset);
    ctrlSetRefWeigts(&rollPid, params->beta, params->gamma);
    ctrlSetSaturation(&rollPid, ROLL_SAT_MAX, ROLL_SAT_MIN);

}

void rgltrSetYawRef(float ref) {
 
    ctrlSetRef(&yawPid, ref);

}

void rgltrSetPitchRef(float ref) {

    ctrlSetRef(&pitchPid, ref);

}

void rgltrSetRollRef(float ref) {

    ctrlSetRef(&rollPid, ref);

}

void rgltrGetQuatRef(Quaternion *ref) {

    if(ref == NULL) { return; }
    quatCopy(ref, &reference);

}

void rgltrSetQuatRef(Quaternion *ref) {

    if(ref == NULL) { return; }
    quatCopy(&reference, ref);

}

void rgltrSetRemoteControlValues(float thrust, float steer, float elevator) {

    rc_outputs.thrust = thrust;
    rc_outputs.steer = steer;
    rc_outputs.elevator = elevator;

}

void rgltrGetState(RegulatorState dst) {

    RegulatorState src;

    src = pbuffGetOldestActive(&reg_state_buff);
    if(src == NULL) { // Return 0's if no unread data
        memset(dst, 0, sizeof(RegulatorStateStruct));
        return; 
    }
    
    memcpy(dst, src, sizeof(RegulatorStateStruct));

    pbuffReturn(&reg_state_buff, src);
    
}

// 7000 cycles
void rgltrRunController(void) {

    bams16_t a_2;
    float a, sina_2, scale;
    float steer, thrust, elevator, yaw_err, pitch_err, roll_err;    
    Quaternion pose, error, conj;
    RegulatorState state;
    
    if(!is_ready) { return; }

    attGetQuat(&pose);      // Retrieve pose estimate
    
    // qref = qerr*qpose
    // qref*qpose' = qerr
    quatConj(&pose, &conj);    
    quatMult(&reference, &conj, &error);

    // q = [cos(a/2), sin(a/2)*[x, y, z]]
    // d[x, y, z] = [q]*a/sin(a/2)    
    
    if(error.w == 1.0) { // a = 0 case
        yaw_err = 0.0;
        pitch_err = 0.0;
        roll_err = 0.0;
    } else {
        a_2 = bams16Acos(error.w); // w = cos(a/2)    
        a = bams16ToFloatRad(a_2*2);
        sina_2 = bams16Sin(a_2);
        scale = a/sina_2;
        yaw_err = error.z*scale;
        pitch_err = error.y*scale;
        roll_err = error.x*scale;
    }
    
    if(reg_mode == REG_REMOTE_CONTROL) {

        steer = rc_outputs.steer;
        thrust = rc_outputs.thrust;
        elevator = rc_outputs.elevator;

    } else if(reg_mode == REG_TRACK){

        steer = runYawControl(yaw_err);
        elevator = runPitchControl(pitch_err);
        thrust = rc_outputs.thrust;
    
    } else {

        steer = 0.0;
        thrust = 0.0;
        elevator = 0.0;
        
    }

    if(yawRateFilter != NULL) {
        pitch_err = dfilterApply(yawRateFilter, pitch_err);
    }

    state = pbuffForceGetIdleOldest(&reg_state_buff);    
    if(state != NULL) {
        state->time = sclockGetLocalTicks();
        memcpy(&state->ref, &reference, sizeof(Quaternion));
        memcpy(&state->pose, &pose, sizeof(Quaternion));                
        state->error.w = a;
        state->error.x = roll_err;
        state->error.y = pitch_err;
        state->error.z = yaw_err;
        state->u[0] = thrust;
        state->u[1] = steer;
        state->u[2] = elevator;
        pbuffAddActive(&reg_state_buff, (void*) state);
    }        

    mcSteer(steer);
    mcThrust(thrust);
    servoSet(elevator);
    
}


// =========== Private Functions ===============================================

static float runYawControl(float yaw) {

    float u;

    if (!ctrlIsRunning(&yawPid)) {
        u = 0.0;
    } else {        
        u = ctrlRunPid(&yawPid, yaw, yawRateFilter);                
    }    

    return u;

}


static float runPitchControl(float pitch) {

    float u;

    if (!ctrlIsRunning(&pitchPid)) {
        u = 0.0;
    } else {
        u = ctrlRunPid(&pitchPid, pitch, pitchRateFilter);
    }

    return u;
    
}

static float runRollControl(float roll) {

    if(!ctrlIsRunning(&rollPid)) {
        return 0.0;
    } else {
        return ctrlRunPid(&rollPid, roll, rollRateFilter);
    }

}

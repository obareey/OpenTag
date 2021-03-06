/* Copyright 2010-2012 JP Norair
  *
  * Licensed under the OpenTag License, Version 1.0 (the "License");
  * you may not use this file except in compliance with the License.
  * You may obtain a copy of the License at
  *
  * http://www.indigresso.com/wiki/doku.php?id=opentag:license_1_0
  *
  * Unless required by applicable law or agreed to in writing, software
  * distributed under the License is distributed on an "AS IS" BASIS,
  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  * See the License for the specific language governing permissions and
  * limitations under the License.
  *
  */
/**
  * @file       /otkernel/~native/system.c
  * @author     JP Norair
  * @version    V1.0
  * @date       2 May 2012
  * @brief      Native kernel & system implementation for DASH7 Mode 2
  * @ingroup    System
  *
  * The current version only works with Mode 2, but it could be elegantly 
  * augmented to work with Mode 1 if there were ever a reason to add Mode 1.
  * The event manager could pick Mode 1 events for Mode 1 sessions.
  * 
  * The "system" is an event manager that does some of the work of the session
  * layer and some of the work of the Data Link Layer (MAC).  There is a list of 
  * events, and each type of event requires a certain type of action.  The 
  * events are also, in most cases, state-based.  In other words, the execution 
  * of one event will often result in the scheduling of another event.
  *
  ******************************************************************************
  */

#include "OT_types.h"
#include "OT_utils.h"
#include "OT_config.h"
#include "OT_platform.h"

//#include "extf_config.h"

#include "native/system_native.h"

#include "auth.h"
#include "buffers.h"
#include "m2_network.h"
#include "m2_transport.h"
#include "external.h"
#include "queue.h"
#include "radio.h"
#include "session.h"
#include "veelite.h"


#define SWDP    OT_FEATURE(WATCHDOG_PERIOD)

OT_INLINE void SYS_WATCHDOG_RUN() {
#if (OT_FEATURE(HW_WATCHDOG) == ENABLED)
    platform_run_watchdog();
#endif    
#if (OT_FEATURE(SW_WATCHDOG) == ENABLED)
    sys.watchdog--;
#endif
}

OT_INLINE void SYS_WATCHDOG_RESET() {
#if (OT_FEATURE(HW_WATCHDOG) == ENABLED)
    platorm_reset_watchdog(SWDP);
#endif    
#if (OT_FEATURE(SW_WATCHDOG) == ENABLED)
    sys.watchdog = SWDP;
#endif
}

OT_INLINE void SYS_WATCHDOG_CHECK() {
#if (OT_FEATURE(HW_WATCHDOG) == ENABLED)
#endif
#if (OT_FEATURE(SW_WATCHDOG) == ENABLED)
    if (sys.watchdog<=0) rm2_kill();
#endif
}



/** Persistent Data Structures 
  */
m2dll_struct    dll;
sys_struct      sys;

typedef void (*fnvv)(void);





/** Internal Routines for system event management
  * ============================================================================
  * Includes both System Events (called by sys_event_manager()) and Radio 
  * Callback Events (called by the radio driver as callbacks).
  */
void sysevt_beacon();
void sysevt_holdscan();
void sysevt_sleepscan();
void sysevt_bscan();
void sysevt_fscan();
void sysevt_receive();
void sysevt_initftx();
void sysevt_initbtx();
void sysevt_txflood();
void sysevt_txcsma();
void sysevt_transmit();

void rfevt_bscan(ot_int scode, ot_int fcode);
void rfevt_frx(ot_int pcode, ot_int fcode);
void rfevt_ftx(ot_int pcode, ot_int scratch);
void rfevt_btx(ot_int flcode, ot_int scratch);





/** Administrative Subroutines
  * ============================================================================
  * Just used to make the code nice-looking or for code-reuse
  */
  
typedef enum {
    TASK_idle       = 0,
    TASK_processing,
    TASK_radio,
    TASK_session,
    TASK_hold,
#   if (M2_FEATURE(ENDPOINT) == ENABLED)
        TASK_sleep,
#   endif
#   if (M2_FEATURE(BEACONS) == ENABLED)
        TASK_beacon,
#   endif
#   if (OT_FEATURE(EXTERNAL_EVENT) == ENABLED)
        TASK_external,
#   endif
    TASK_terminus
} Task_Index;
  
Task_Index sub_clock_tasks(ot_uint elapsed);

void    sub_scan_channel(idletime_event* idlevt, ot_u8 SS_ISF);

void    sub_sys_flush();
ot_u8   sub_default_idle();
void    sub_idlevt_ctrl(idletime_event* idlevt, ot_long* eta, ot_u8 sequence_id);
//void    sub_worevt_ctrl(wakeon_event* worevt, ot_long* eta);





/** Flow & Congestion Control Subroutines
  * ============================================================================
  */
  
/** @brief Performs subnet filtering (per Mode 2 spec) and returns status
  * @retval ot_bool     True/False on received frame subnet passes/fails filter
  * @ingroup System
  */
ot_bool sub_mac_filter();


/** @brief Scrambles the response-channel-list in order to improve collision
  *        avoidance when multiple response channels are available.
  * @ingroup System
  */
void sub_csma_scramble();


/** @brief Initializes the flow/congestion control sequence
  * @retval ot_uint     Initial TX backoff, in ticks
  * @ingroup System
  */
ot_uint sub_fcinit();


/** @brief Evaluates the TX slot usage based on the quality of the query
  * @param  query_score (ot_int) To be defined
  * @retval none        
  * @ingroup System
  * @note Optional, not implemented yet
  */
void sub_fceval(ot_int query_score);


/** @brief Processes the flow/congestion control sequence following initialization
  * @retval ot_uint     Subsequent TX backoff, in ticks
  * @ingroup System
  */
ot_uint sub_fcloop();


/** @brief Initializes a RIGD sequence and determines initial TX offset
  * @retval ot_uint     Number of ticks until TX for new slot should commence
  * @ingroup System
  * @sa sub_fcinit(), sub_rigd_nextslot()
  *
  * @note JP pronounces this "rig-dee."  RIGD is one reason why DASH7 Mode 2
  * CSMA-CA shits all over 802.15.4 CSMA-CA.
  *
  * This is called by sub_fcinit() when RIGD is specified by the request.  RIGD
  * is "Random Increase Geometric Decay" which divides the active contention 
  * period (dll.comm.tca) into subslots of decaying duration and picks random
  * TX offsets within each subslot.
  */
ot_uint sub_rigd_newslot();


/** @brief Continues an ongoing RIGD sequence and determines subslot TX offset
  * @retval ot_uint     Number of ticks until TX for next slot should commence
  * @ingroup System
  * @sa sub_fcloop(), sub_rigd_newslot()
  *
  * This is called by sub_fcloop() and is much like sub_rigd_newslot().
  */
ot_uint sub_rigd_nextslot();


/** @brief Picks a TX offset based on the AIND (or RAIND) method
  * @retval ot_uint     Number of ticks until TX for next slot should commence
  * @ingroup System
  * @sa sub_fcinit(), sub_fcloop()
  *
  * @note JP pronounces these "ain-dee" and "rain-dee"
  *
  * This is called by sub_fcinit() and sub_fcloop() when a RAIND or AIND process
  * is moving along.  RAIND means "Randomized Additive Increase No Decay," and
  * AIND is just without the "Randomized."  Basically, it uses a fixed subslot
  * size which can be initially offset with a random value (or not).
  */
ot_uint sub_aind_nextslot();









#if (OT_FEATURE(CAPI) == ENABLED)

/** OTAPI Server System Functions
  * ============================================================================
  */
  
#define OTAPI_Q     rxq

ot_u16 otapi_new_session(session_tmpl* s_tmpl) {
#if (SYS_SESSION == ENABLED)
    m2session* session;
    
    /// Flush-out any sessions that might be blocking this new session
    session_flush();
    
    /// Create an ad-hoc session at the top of the stack, and verfy that it was
    /// successfully added to the stack.  (session always begins with req tx)
    session = session_new(0, (M2_NETSTATE_INIT | M2_NETSTATE_REQTX), s_tmpl->channel);
    
    if (session == NULL) {
        //OT_LOGFAIL_SYS(-0x10-1); 
        return 0;
    }
    
    session->subnet         = (dll.netconf.subnet & ~s_tmpl->subnetmask) | \
                              (s_tmpl->subnet & s_tmpl->subnetmask);
    session->flags          = (dll.netconf.dd_flags & ~s_tmpl->flagmask) | \
                              (s_tmpl->flags & s_tmpl->flagmask);
                              
    /// Amount of time to try to do the CA to start the session.
    dll.comm.tc             = s_tmpl->timeout;
    
    /// These are fixed default settings for all session inits
    dll.comm.redundants     = 1;
    dll.comm.tx_channels    = 1;
    dll.comm.rx_channels    = 1;
    dll.comm.scratch[0]     = session->channel;
    dll.comm.tx_chanlist    = &dll.comm.scratch[0];
    dll.comm.rx_chanlist    = &dll.comm.scratch[0];
    dll.comm.csmaca_params  = (M2_CSMACA_NA2P | M2_CSMACA_MACCA);

    /// return a session id of sorts
    return *( ((ot_u16*)session)+1 );
    
    ///@note LLDP/DFVM/User will need to invoke the event manager
#else
    return 0;
    
#endif
}



ot_u16 otapi_open_request(addr_type addr, routing_tmpl* routing) {
/// Set the header if the session is valid.  Also conditionally write the header
/// depending on the address type (a parameter).  
    if (session_count() >= 0) {
        m2session* session;
        session = session_top();
    
        // Unicast/Anycast support routing, so copy the supplied template
        if ((addr & 0x40) == 0) {   
            platform_memcpy((ot_u8*)&m2np.rt, (ot_u8*)routing, sizeof(routing_tmpl));
        }

        // Load the header: last argument is for NACK (0 means normal request)
        m2np_header(session, (ot_u8)addr, 0);
        return 1;
    }
    return 0;
}



ot_u16 otapi_close_request() {
/// Set the footer if the session is valid
    if (session_count() >= 0) {
        m2np_footer( session_top() );
        return 1;
    }
    return 0;
}



ot_u16 otapi_start_flood(ot_u16 flood_duration) {
#if (SYS_FLOOD == ENABLED)
    m2session*  session;
    
    session = session_top();

    // If the user put in 0, don't flood.
    if (flood_duration == 0) {
        return otapi_start_dialog();
    }
    
    // Otherwise, see if a flood is possible (if not, kill session and return 0)
    if (m2advp_init_flood(session, flood_duration) < 0) {
        session_pop();
        return 0;
    }
    
    // At this point, flood is prepared.  Time to run it.
    sysevt_initbtx();
    return sys_event_manager(0);

#else
    return 0;
#endif
}





ot_u16 otapi_start_dialog() {
/// Stop any ongoing processes and seed the event for the event manager.  The
/// radio killer will work in all cases, but it is bad form to kill sessions
/// that are moving data.  That is, qualify your app event by making sure that
/// sys.mutex is less than 2.
    if (sys.mutex != 0) {
        sys.mutex = 0;
        rm2_kill();
    }
    platform_ot_preempt();
    return 1;
}




ot_u16 otapi_sysinit() {
    sys_refresh();
    return 1;
}

#endif









/** System Core Functions
  * ============================================================================
  */
  
ot_bool sys_loadapp_null(void) { return False; }

#ifndef EXTF_sys_init
void sys_init() {    
/// Set system callbacks to null (if callbacks enabled)

#   if ((OT_FEATURE(SYSKERN_CALLBACKS) == ENABLED) && !defined(EXTF_sys_sig_loadapp))
        sys.loadapp = &sys_loadapp_null;
#   endif

#	if ((OT_FEATURE(SYSKERN_CALLBACKS) == ENABLED) && !defined(EXTF_sys_sig_panic))
        sys.panic = &otutils_sig_null;
#   endif

#   if ((OT_FEATURE(SYSRF_CALLBACKS) == ENABLED) && !defined(EXTF_sys_sig_rfainit))
        sys.evt.RFA.init = &otutils_sig_null;
#   endif
#   if ((OT_FEATURE(SYSRF_CALLBACKS) == ENABLED) && !defined(EXTF_sys_sig_rfaterminate))
        sys.evt.RFA.terminate = &otutils_sig2_null;
#   endif

#   if ((OT_FEATURE(SYSIDLE_CALLBACKS) == ENABLED) && !defined(EXTF_sys_sig_hssprestart))
        sys.evt.HSS.prestart = &otutils_sigv_null;
#   endif
        
#   if ((M2_FEATURE(ENDPOINT) == ENABLED) && \
		(OT_FEATURE(SYSIDLE_CALLBACKS) == ENABLED) && \
		!defined(EXTF_sys_sig_sssprestart))
            sys.evt.SSS.prestart = &otutils_sigv_null;
#   endif

#   if ((M2_FEATURE(BEACONS) == ENABLED) && \
		(OT_FEATURE(SYSIDLE_CALLBACKS) == ENABLED) && \
		!defined(EXTF_sys_sig_btsprestart))
            sys.evt.BTS.prestart = &otutils_sigv_null;
#   endif

#   if (OT_FEATURE(EXTERNAL_EVENT) == ENABLED)
        sys.evt.EXT.event_no = 0;
#       if ((OT_FEATURE(SYSIDLE_CALLBACKS) == ENABLED) && \
		    !defined(EXTF_sys_sig_extprocess))
            sys.evt.EXT.prestart = &otutils_sigv_null;
#       endif
#	endif

    /// Initialize non-platform modules
    network_init();
    m2qp_init();
    auth_init();
    ext_init();
    
    /// Load the Network settings from ISF 0 to the dll.netconf buffer, reset
    /// the session, and send system to idle.
    sys_refresh();
}
#endif




#ifndef EXTF_sys_refresh
void sys_refresh() {
    Twobytes scratch;
    vlFILE* fp;
    
    /// Open Device Features ISF and read the settings bytes
    fp = ISF_open_su( 0x00 );
    scratch.ushort          = vl_read(fp, 2);
    dll.netconf.subnet      = scratch.ubyte[0];
    dll.netconf.b_subnet    = scratch.ubyte[1];
    scratch.ushort          = vl_read(fp, 6);
    dll.netconf.dd_flags    = scratch.ubyte[0];
    dll.netconf.b_attempts  = scratch.ubyte[1];
    dll.netconf.active      = vl_read(fp, 4);
    dll.netconf.hold_limit  = vl_read(fp, 8);   ///@todo endian conversion
    vl_close(fp);

    sub_sys_flush();
}
#endif




#ifndef EXTF_sys_change_settings
void sys_change_settings(ot_u16 new_mask, ot_u16 new_settings) {   
    vlFILE* fp_active;
    vlFILE* fp_supported;
    
    // Get Active Settings, Get Supported Settings,
    // Mask-out unsupported settings, apply to new active settings
    ///@todo assert fp
    fp_active           = ISF_open_su( 0x00 );
    fp_supported        = ISF_open_su( 0x01 );
    new_mask           &= vl_read(fp_supported, 8);
    dll.netconf.active  = vl_read(fp_active, 4);
    new_settings       &= new_mask;
    dll.netconf.active &= ~new_mask;
    dll.netconf.active |= new_settings;
    
    // Write the new settings to the ISF 0
    vl_write(fp_active, 4, dll.netconf.active);
    vl_close(fp_active);
    vl_close(fp_supported);
    
    // Flush the System of all Sessions and Events, and restart it
    sub_sys_flush();
}
#endif




#ifndef EXTF_sys_goto_off
void sys_goto_off() {
/// Off Routine: Kill everything and shut-down all events.  Use sys_init to
/// Turn back on.  External events can still initiate TX.
    session_init();
    
    sys.evt.RFA.event_no = 0;
    sys.evt.HSS.event_no = 0;
    
#   if (M2_FEATURE(ENDPOINT) == ENABLED)
        sys.evt.SSS.event_no = 0;
#   endif
#   if (M2_FEATURE(BEACONS) == ENABLED)    
        sys.evt.BTS.event_no = 0;
#   endif


#   if (OT_FEATURE(EMBEACON) == ENABLED)
    ///@todo Emergency Beacon Event control
#   endif    
}
#endif




#ifndef EXTF_sys_goto_sleep
void sys_goto_sleep() {
/// Only go to Sleep if the device is configured as an endpoint.  Otherwise,
/// go to Hold instead.
#if (M2_FEATURE(ENDPOINT) != ENABLED)
    sys_goto_hold();

#else
//#   if ((M2_FEATURE(GATEWAY) == ENABLED) || (M2_FEATURE(SUBCONTROLLER) == ENABLED))
//        if ((dll.netconf.active & M2_SET_ENDPOINT) == 0) {
//            sys_goto_hold();
//        }
//        else
//#   endif
        
    //if (session_count() >= 0) {
    //    m2session* session;
    //    session = session_top();
    //    session->netstate  &= ~M2_NETFLAG_HOLD;
    //    session_flush();
    //}
    
    /// Manage scheduler, if it is enabled and activated
#   if (M2_FEATURE(RTC_SCHEDULER) == ENABLED)
        if (sys.evt.SSS.sched_id != 0) {
            sub_activate_scheduler( &sys.evt.SSS.sched_id );
        }
#   endif
      
    /// Event Management
    /// - Set the Sleep Scan cursor to 0, 
    /// - Set next sleep event to immediately (0)
    /// - Enable Sleep event, disable others
    sys.evt.SSS.cursor      = 0;
    sys.evt.SSS.event_no    = 1;
    sys.evt.HSS.event_no    = 0;
    sys.evt.RFA.event_no    = 0;
#endif
}
#endif




#ifndef EXTF_sys_goto_hold
void sys_goto_hold() {
/// All devices can go to the hold state, but only endpoints treat it as a true
/// Hold state (where session is put on hold).  The others, it is just idle.

    /// If this is an endpoint going to hold, HOLD the session stack
    /// Otherwise, flush the expired session(s)
#   if (M2_FEATURE(ENDPOINT) == ENABLED)
        //if (dll.netconf.active & M2_SET_ENDPOINT) {
        //    if (session_count() < 0) {      // safety: is session stack is fuct, go to sleep
        //        sys_goto_sleep();
        //        return;
        //    }
        //    {
        //        m2session* session;
        //        session = session_top();
        //        session->netstate  |= M2_NETFLAG_HOLD;
        //    }
        //    
        //}
#       if ((M2_FEATURE(GATEWAY) == ENABLED) || \
            (M2_FEATURE(SUBCONTROLLER) == ENABLED) || \
            (M2_FEATURE(BLINKER) == ENABLED))
            //else {
            //    session_flush();
            //}
#       endif
#   else
        //session_flush();
#   endif

    /// Manage scheduler, if it is enabled and activated
#   if (M2_FEATURE(RTC_SCHEDULER) == ENABLED)
        if (sys.evt.HSS.sched_id != 0) {
            sub_activate_scheduler( &sys.evt.HSS.sched_id );
        }
#   endif    
    
    /// Manage Events:
    sys.evt.HSS.event_no    = 1;
#   if (M2_FEATURE(ENDPOINT) == ENABLED)
        sys.evt.SSS.event_no    = 0;
#   endif
    sys.evt.RFA.event_no    = 0;
}
#endif




#ifndef EXTF_sys_panic
void sys_panic(ot_u8 err_code) {
/// Turn off all idle-time events and load an emergency beacon into the queue
/// (emergency beacon data should be stored somewhere in static memory).
    dll.idle_state = 0;
    session_flush();
    sys_idle();
    platform_flush_gptim();

#   if ((OT_FEATURE(SYSKERN_CALLBACKS) == ENABLED) && \
        !defined(EXTF_sys_sig_panic))
        sys.panic(err_code);
        
#   elif defined(EXTF_sys_sig_panic)
        sys_sig_panic(err_code);
#   endif
}
#endif




#ifndef EXTF_sys_idle
void sys_idle() {
/// Idle Routine
/// <LI> Make sure the radio is powered down </LI>
/// <LI> Shift state tracker to idle </LI>
/// <LI> Vector to the appropriate version of Idle, based on setting </LI>
    static const ot_sub call_table[4] = {
        &sys_goto_off,
        &sys_goto_sleep,
        &sys_goto_hold,
        &sys_goto_hold
    };

    radio_gag();
    radio_sleep();
    sys.mutex = 0;
    
//    switch (dll.idle_state & 0x03) {
//        case 0: sys_goto_off();     break;
//        case 1: sys_goto_sleep();   break;
//        case 2: sys_goto_hold();    break;
//        case 3: break;
//    }
    
    call_table[dll.idle_state & 3]();
}
#endif




#ifndef EXTF_sys_default_csma
ot_u8 sys_default_csma(ot_u8 chan_id) {
    chan_id &= 0x30;
    return (((chan_id == 0x00) || (chan_id == 0x30)) << 2);
}
#endif




#ifndef EXTF_sys_quit_rf
OT_INLINE void sys_quit_rf() {
    sys.evt.RFA.event_no = 0;
}
#endif




#ifndef EXTF_sys_set_mutex
OT_INLINE void sys_set_mutex(ot_uint set_mask) {
    sys.mutex |= (ot_u8)set_mask;
}
#endif




#ifndef EXTF_sys_clear_mutex
OT_INLINE void sys_clear_mutex(ot_uint clear_mask) {
    sys.mutex &= ~clear_mask;
}
#endif




#ifndef EXTF_sys_get_mutex
OT_INLINE ot_int sys_get_mutex() {
    return sys.mutex;
}
#endif






///@todo there is potential to optimize this function, or put it inside sub_clock_tasks()
OT_INLINE void sub_next_event(ot_long* event_eta) {
    static const ot_u8 isf_lut[] = {
        ISF_ID(hold_scan_sequence),
#       if (M2_FEATURE(ENDPOINT) == ENABLED)
            ISF_ID(sleep_scan_sequence),
#       endif
#       if (M2_FEATURE(BEACONS) == ENABLED)
            ISF_ID(beacon_transmit_sequence),
#       endif
        0xFF
    };
    
    ot_int i;
    *event_eta = 65535;

    for (i=(IDLE_EVENTS-1); i>=0; i--) {
    	if (sys.evt.idle[i].event_no != 0) {
    		sub_idlevt_ctrl(&sys.evt.idle[i], event_eta, isf_lut[i]);
    	}
    }
}




#ifndef EXTF_sys_event_manager
ot_uint sys_event_manager(ot_uint elapsed) {
/// Check the event list, and act on them as necessary.  If an event succeeds,
/// then the sys.evt.process will be put to some other function in the SYS.   
    
    do {
        /// 1. Flush the timer.  The amount of time the task uses is clocked, 
        ///    and subsequently it is subtracted from all the task timers.
        platform_flush_gptim();
        
        /// 2. Check the system watchdog to make sure there isn't a task going
        ///    haywire.  The OT kernel is mostly self-cleaning, but radio 
        ///    operations are usually interrupt-driven, and they can be made
        ///    fault-tolerant by using the watchdog.
        SYS_WATCHDOG_CHECK();
    
        /// 3. Clock idle time events, and any sessions in the session stack.
        ///    The highest priority task that needs servicing will be returned.
        ///    The time required to clock the events is assumed to be negligible
        ///    (it is at most 50 instructions, I would guess)
        switch (sub_clock_tasks(elapsed)) {
        
            // Completely Idle Time:
            // Run an external process that can manipulate the kernel.  If the external
            // process (loadapp) does not exist or it does not do anything, then EXIT
            // the kernel and return estimated-time-of-arrival (eta) of next known event.
            case TASK_idle: {
                ot_long event_eta;
                
                if (session_count() >= 0) {
                    m2session* session;
                    session = session_top();
                    if (session->netstate & M2_NETSTATE_CONNECTED) {
                        return session->counter;
                    }
                    event_eta = session->counter;
                }
               
#               if ((OT_FEATURE(SYSKERN_CALLBACKS) == ENABLED) && \
                    !defined(EXTF_sys_sig_loadapp) )
                    // The "loadapp" callback should return "True" (non-zero) when 
                    // it wants the kernel to continue.  It usually also needs to
                    // create a session.
                    if (sys.loadapp()) break;
#               elif defined(EXTF_sys_sig_loadapp)
                    if (sys_sig_loadapp()) break;
#               endif
                
                sub_next_event(&event_eta);
                if (event_eta <= 0) {
                    break;
                }
                return (ot_uint)event_eta;
            } 
        
            
            // Packet Processing Task: 
            // NOT assumed to finish instantly: in order to maintain good timeslot
            // precision, the RF slop and packet processing need to be clocked, too.
            case TASK_processing: {
                m2session* session;
                ot_int proc_score;
                session             = session_top();
                session->counter    = 0;
                proc_score          = network_route_ff(session);
                
                /// If the score is negative, then the packet is not
                /// meant for this device.  Else, prepare for TX and 
                /// potentially a follow-up listen.
                if (proc_score >= 0) {
                    sub_fceval(proc_score);
                    sys.evt.hold_cycle  = 0;
                    dll.idle_state      = M2_MACIDLE_HOLD;

                    /// If the Listen flag is high, then clone the session to 
                    /// a time in the future, when it will listen.  But first,
                    /// wipe-out any sessions that could get in the way
                    if (session->flags & M2FI_LISTEN) {
                        m2session* s_clone;

                        session_refresh(dll.comm.tc);
                        session_drop();
                    
                        /// Offset the Tc if tryping to beat timing slop,
                        //dll.comm.tc -= OFFSET VALUE ... TBD
                    
                        s_clone = session_new( \
                                (dll.comm.tc), 
                                (M2_NETSTATE_REQRX | M2_NETSTATE_ASSOCIATED),
                                (dll.comm.rx_chanlist[0])   );
                    
                        s_clone->dialog_id      = session->dialog_id;
                        s_clone->subnet         = session->subnet;
                        s_clone->channel        = session->channel;
                        dll.comm.redundants     = 0;
                        dll.comm.rx_chanlist    = &dll.comm.scratch[1];
                        dll.comm.rx_chanlist[0] = session->channel;   
                        dll.comm.rx_timeout     = 10;
                        dll.comm.tc            -= rm2_pkt_duration(txq.length);
                    }
                }
                sys.mutex &= ~SYS_MUTEX_PROCESSING;
            } break;
            
        
            // RF Management Task (has sub-threads)
            // subthread 1: If radio is TX/RX data, wait for it to finish
            // subthread 2: Enter TX process
            // subthread 3: Exit RX-Listening process (timeout)
            // subthread 4: Radio is known to be doing something: return event_eta
            case TASK_radio: { 
                if (sys.evt.RFA.nextevent <= 0) {
                	if (sys.evt.RFA.event_no < 3) {	// RX tasks
                		sysevt_receive();           // Manage RX timeouts in SW
                	}
                	else if (sys.evt.RFA.event_no < 5) {
                		sysevt_txcsma();            // Manage CSMA process in SW
                	}
                	else {
                		SYS_WATCHDOG_RUN();         // Wait for TX to complete
                        return 1;                   // come back in 1 tick
                	}
                    break;
                }
                return sys.evt.RFA.nextevent;
            } 
            
        
            // Session Creation Task (not actually sub-threads)
            case TASK_session: {
                static const ot_sub call_table[4] = {
                        &sysevt_initftx,
                        &sysevt_fscan,
                        &sysevt_initbtx,
                        &sysevt_bscan
                    };
                ot_u8 call_code;

                session_drop();
                dll.idle_state  = sub_default_idle();
                call_code       = (session_netstate() >> 5);
                if (call_code & 4) {    //scrap session
                    session_pop();
                    sys_idle();
                }
                else {
                    call_table[call_code]();
                }
            } break;
        
        
            // Hold Scan Management (version for endpoint-enabled devices is special)
#           if ((SYS_RECEIVE == ENABLED) && (M2_FEATURE(ENDPOINT) == ENABLED))
            case TASK_hold: {
                sys.evt.hold_cycle += (ot_uint)(sys.evt.HSS.cursor == 0);
                if (((dll.netconf.active & M2_SET_CLASSMASK) == M2_SET_ENDPOINT) && \
                    (sys.evt.hold_cycle == dll.netconf.hold_limit)) {
                    sys_goto_sleep();
                    sysevt_sleepscan();
                }
                else {
                    sysevt_holdscan();
                }
            } break;
        
#           elif (SYS_RECEIVE == ENABLED)
            case TASK_hold: 
                sysevt_holdscan();
                break;
#           endif
        
        
            // Sleep Scan Management
#           if (M2_FEATURE(ENDPOINT) == ENABLED)
            case TASK_sleep: 
                sysevt_sleepscan();
                break;
#           endif

        
            // Beacon Transmit Managment
#           if (M2_FEATURE(BEACONS) == ENABLED)
            case TASK_beacon: 
                sysevt_beacon();
                break;
#           endif


            // External Event Manager
#           if (OT_FEATURE(EXTERNAL_EVENT) == ENABLED)
            case TASK_external:
#           ifdef EXTF_sys_sig_extprocess
                sys_sig_extprocess(NULL);
#           else
                sys.evt.EXT.prestart(NULL);
#           endif
                break;
#           endif


            // Task error
            default: {
                sys_panic(64); ///@todo Pick an appropriate error code
            } break;
        }
        
        /// Clear [optional] watchdog when Radio Tasks are inactive
        SYS_WATCHDOG_RESET();
    
        elapsed = platform_get_gptim();
    }
    while (1);
}
#endif




#ifndef EXTF_sys_clock_tasks
OT_INLINE Task_Index sub_clock_tasks(ot_uint elapsed) {
    ot_int i;
    Task_Index output = TASK_idle;

    // Clock Tca & RX timeout
    //dll.comm.rx_timeout -= elapsed;
    dll.comm.tca        -= elapsed;
    //dll.comm.tc         -= elapsed;

    for (i=(IDLE_EVENTS-1); i>=0; i--) {
        sys.evt.idle[i].nextevent -= (ot_long)elapsed;
        if ((sys.evt.idle[i].event_no != 0) && (sys.evt.idle[i].nextevent <= 0))
            output = TASK_hold+i;
    }

    // Clock sessions (Priority 3)
    if (session_refresh(elapsed))
        output = TASK_session;

    // Clock the Radio Event (Priority 2)
    if (sys.evt.RFA.event_no != 0) {
        output                  = TASK_radio;
        sys.evt.RFA.nextevent  -= elapsed;
    }

    // Do Immediate Packet Processing (Priority 1)
    if (sys.mutex & SYS_MUTEX_PROCESSING)
        output = TASK_processing;

    return output;
}
#endif



/** System Events <BR>
  * ============================================================================
  */


/** Idle Time Events <BR>
  * ======================================================================= <BR>
  * - Scanning and Beaconing                                                <BR>
  * - Set up pending radio RX or TX events                                  <BR>
  * - Include callbacks that run before they start (if callbacks enabled)   <BR>
  * - Generally run very fast (0 ticks, or negligible)
  */

OT_INLINE void sysevt_holdscan() {
#if (M2_FEATURE(BLINKER) != ENABLED)
    sub_scan_channel(&sys.evt.HSS, ISF_ID(hold_scan_sequence));
#endif
}




OT_INLINE void sysevt_sleepscan() {
/// See implementation notes for sysevt_holdscan
#if (M2_FEATURE(ENDPOINT) == ENABLED)
    sub_scan_channel(&sys.evt.SSS, ISF_ID(sleep_scan_sequence));
#endif
}




void sub_scan_channel(idletime_event* idlevt, ot_u8 SS_ISF) {
/// Like with the Beacon event processor, the scan event processor currently
/// is expected to run in negligible time: it is non-blocking and the actual
/// reception is another event, so this is generally accurate.  In the future
/// elapsed time could be taken into account without too much trouble.
#if ((M2_FEATURE(GATEWAY) == ENABLED) || \
     (M2_FEATURE(SUBCONTROLLER) == ENABLED) || \
     (M2_FEATURE(ENDPOINT) == ENABLED))
    ot_u8       s_channel;
    ot_u8       s_flags;
    
    {
        Twobytes    scratch;
        vlFILE*     fp;
    
#       if (OT_FEATURE(SYSIDLE_CALLBACKS) == ENABLED)
            idlevt->prestart( (void*)idlevt );
#       endif
    
        /// Load scan data from the config file (Hold scan or Sleep scan)
        fp = ISF_open_su( SS_ISF );
        ///@todo assert fp
    
        /// Pull channel ID and Scan flags
        scratch.ushort          = vl_read(fp, idlevt->cursor);
        s_channel               = scratch.ubyte[0];
        s_flags                 = scratch.ubyte[1];
        
        /// Set the next idle event from the two-byte Next Scan field
#       ifdef __BIG_ENDIAN__
            idlevt->nextevent   = vl_read(fp, idlevt->cursor+=2 );
#       else
            scratch.ushort      = vl_read(fp, (idlevt->cursor)+=2 );
            scratch.ushort      = (scratch.ushort << 8) | (scratch.ushort >> 8);
            					///@todo implement inline swap function in platform
            idlevt->nextevent   = (ot_long)scratch.ushort;
#       endif
        
        /// Advance cursor to next datum, go back to 0 if end of sequence
        idlevt->cursor += 2;
        if (idlevt->cursor >= fp->length) {
            idlevt->cursor = 0;
        }
        //idlevt->cursor *= (idlevt->cursor < fp->length);
        vl_close(fp);
    }
    
    /// Perform the scan                                                    <BR>
    ///  - b5:0 of the scan flags is the normal scan timeout                <BR>
    ///  - b6 of the scan flags enables 1024x multiplier on scan timeout    <BR>
    ///  - b7 of the scan flags is foreground (0), background (1)
    dll.comm.rx_timeout     = otutils_calc_timeout(s_flags);
    dll.comm.redundants     = 0;
    dll.comm.rx_channels    = 1;
    dll.comm.rx_chanlist    = &dll.comm.scratch[1];
    dll.comm.scratch[1]     = s_channel;
        
    /// Background Scan or Foreground Scan is based on flags
    s_flags = (s_flags & 0x80) ? \
                (M2_NETSTATE_REQRX | M2_NETSTATE_INIT |M2_NETFLAG_FLOOD ) : \
                (M2_NETSTATE_REQRX | M2_NETSTATE_INIT);

    session_new(0, s_flags, s_channel);
#endif
}




void sysevt_beacon() {
/// The beacon event is probably finished, from a feature perspective.  It
/// behaves like a proper event, returning quickly without much processing,
/// and it calls the initftx event when it is ready to transmit.  If beacon
/// process runtime is taken into account later (now it is negligible), some
/// simple edits can be made to this function, which are noted.
#if (M2_FEATURE(BEACONS) == ENABLED)
    vlFILE*     fp;
    m2session*  session;
    ot_u8       beacon_params;
    Queue       beacon_queue;
    Fourbytes   bq_data;
    Twobytes    scratch;

    /// Open BTS ISF Element and read the beacon sequence.  Make sure there
    /// is a beacon file of non-zero length and that beacons are presently
    /// enabled.  Otherwise, in 64 seconds it will check again.  The value
    /// 64s is arbitrary.  You can change it or asynchronously pre-empt a
    /// beacon restart by flushing the system.
    fp = ISF_open_su( ISF_ID(beacon_transmit_sequence) );
    if ((dll.netconf.b_attempts == 0) || (fp->length == 0)) {
        vl_close(fp);
        sys.evt.BTS.nextevent = 65535;  ///@todo make this an app-config parameter
        return;
    }
    
    // First 2 bytes: Chan ID, Cmd Code
    // - Setup beacon ad-hoc session, on specified channel
    //   (ad hoc sessions never return NULL)
    // - Assure cmd code is always Broadcast & Announcement
    scratch.ushort          = vl_read(fp, sys.evt.BTS.cursor);
    session                 = session_new(  0,
    		                                (M2_NETSTATE_INIT | M2_NETFLAG_FIRSTRX),
    		                                scratch.ubyte[0]  );
    session->subnet         = dll.netconf.b_subnet;
    beacon_params           = scratch.ubyte[1];
    session->flags          = (dll.netconf.dd_flags & ~0x30);
    session->flags         |= (beacon_params & 0x30);
        
    // Second & Third 2 bytes: ISF Call Template
    ///@todo Agaidi has the q init going to 32.  I need to find why
    q_init(&beacon_queue, &bq_data.ubyte[0], 4);
    bq_data.ushort[0]       = vl_read(fp, sys.evt.BTS.cursor+=2);
    bq_data.ushort[1]       = vl_read(fp, sys.evt.BTS.cursor+=2);
        
    // Last 2 bytes: Next Scan ticks
#   ifdef __BIG_ENDIAN__
        sys.evt.BTS.nextevent   = vl_read(fp, sys.evt.BTS.cursor+=2);
#   else
        scratch.ushort          = vl_read(fp, sys.evt.BTS.cursor+=2);
        scratch.ushort          = (scratch.ushort << 8) | (scratch.ushort >> 8);
        sys.evt.BTS.nextevent   = (ot_long)scratch.ushort;
#   endif
        
    // - Move cursor onto next beacon period, 
    // - loop cursor if it is past the length of the list
    // - and close file
    sys.evt.BTS.cursor += 2;
    if (sys.evt.BTS.cursor >= fp->length) {
        sys.evt.BTS.cursor = 0;
    }
    vl_close(fp);
    
    ///Start building the beacon packet
    m2np_header(session, 0x40, 0);
    q_writebyte(&txq, 0x20 + (beacon_params & 1));
    {
        ot_u8 increment;
        *txq.putcursor  = (beacon_params & 0x04);
        increment       = ((beacon_params & 0x04) != 0);
        txq.putcursor  += increment;
        txq.length     += increment;
    }

    /// Setup the comm parameters, if the channel is available
    /// <LI> tx_eirp, cs_rssi, and cca_rssi are set by the radio module </LI>
    /// <LI> during the CSMA-CA process </LI>
    dll.comm.tc             = M2_PARAM_BEACON_TCA;
    dll.comm.rx_timeout     = (beacon_params & 0x02) ? \
                                0 : rm2_default_tgd(session->channel); 
    q_writebyte(&txq, (ot_u8)dll.comm.rx_timeout);

    dll.comm.csmaca_params  = sys_default_csma(session->channel);
    dll.comm.csmaca_params |= (beacon_params & 0x04);
    dll.comm.csmaca_params |= (M2_CSMACA_NA2P | M2_CSMACA_MACCA);
    dll.comm.redundants     = dll.netconf.b_attempts;              
    dll.comm.tx_channels    = 1;
    dll.comm.rx_channels    = 1;
    dll.comm.tx_chanlist    = &dll.comm.scratch[0];
    dll.comm.rx_chanlist    = &dll.comm.scratch[1];
    dll.comm.scratch[0]     = session->channel;
    dll.comm.scratch[1]     = session->channel;
        
    /// Finish building the beacon packet.  If the 
    if (m2qp_isf_call((beacon_params & 1), &beacon_queue, AUTH_GUEST) >= 0) {
        m2np_footer(session);
#       if ((OT_FEATURE(SYSIDLE_CALLBACKS) == ENABLED) &&\
            !defined(EXTF_sys_sig_btsprestart)  )
            sys.evt.BTS.prestart( (void*)&sys.evt.BTS );
#       elif defined(EXTF_sys_sig_btsprestart)
            sys_sig_btsprestart( (void*)&sys.evt.BTS );
#       endif
    }
    else {
        session_pop();
    }
#endif
}








/** Radio I/O Events
  * ============================================================================
  * - Initialize RX, Initialize TX, plus variations                         <BR>
  * - Negotiate radio callbacks with radio core driver                      <BR>
  * - Include system callbacks that run on init (if callbacks enabled)      <BR>
  * - Include system callbacks that run on completion (if callbacks enabled)<BR>
  * - Non-blocking, although sometimes system event handler is paused
  */


void sysevt_receive() {
/// This function just operates the timing-out of the RX.  RX is forced into
/// timeout when there is no data being received OR if the MAC is operating
/// under the A2P regime, which uses strict time-slots.

/// @note if you want to use a built-in RX termination or polling timer inside
/// the radio core (some devices have these), be very careful.  If the radio
/// timer gets out of sync with the kernel (i.e. if your core timer isn't very
/// accurate), then it is going to cause all sorts of timing glitches.  Giving
/// the kernel control over listening makes the system more stable and better
/// synchronized.  Usage of HW RX timer is mostly useful for very specific
/// applications using very custom builds of OpenTag.

#if (RF_FEATURE(RXTIMER) == DISABLED)
	if (((sys.mutex & SYS_MUTEX_RADIO_DATA) == 0) || \
		(dll.comm.csmaca_params & M2_CSMACA_A2P)  ) {
        rm2_rxtimeout_isr();
	}
#else
        // Add a little bit of time in case the radio timer is a bit slow.
        sys.evt.RFA.nextevent = 10;
        sys.evt.RFA.event_no  = 0;
#endif
}





void sysevt_bscan() {
/// The bscan event (Background Scan) is used when the acquired HSS or SSS
/// period (from some offset in ISF 05 or 04 respectively) specifies a bscan as
/// opposed to an fscan.  bscan is sessionless, so it excutes directly from the
/// channel scan event process (it is therefore a child process).
///
/// Background scans happen quickly (1-10 ticks) unless you decide to jack-up 
/// the bscan redundancy to a high amount (best practice is <=5, typ 3).  Bscans
/// block the system event handler while underway.
#if ((M2_FEATURE(GATEWAY) == ENABLED) || \
     (M2_FEATURE(SUBCONTROLLER) == ENABLED) || \
     (M2_FEATURE(ENDPOINT) == ENABLED))

#   if ((OT_FEATURE(SYSRF_CALLBACKS) == ENABLED) &&\
        !defined(EXTF_sys_sig_rfainit)  )
        sys.evt.RFA.init(1);
#   elif defined(EXTF_sys_sig_rfainit)
        sys_sig_rfainit(1);
#   endif
    
    sys.evt.RFA.event_no    = 1;
    sys.evt.RFA.nextevent   = dll.comm.rx_timeout;
    sys.mutex               = SYS_MUTEX_RADIO_LISTEN;
    rm2_rxinit_bf(dll.comm.rx_chanlist[0], &rfevt_bscan);
#endif
}




void rfevt_bscan(ot_int scode, ot_int fcode) {
/// bscan reception radio-core event callback: called by radio core driver when
/// the bscan process terminates, either due to success or failure

    // CRC Failure (or init), retry
    if ((scode == -1) && (dll.comm.redundants != 0)) {
        rm2_rxinit_bf(dll.comm.rx_chanlist[0], &rfevt_bscan);    //non-blocking
    }
    
    // Do not retry (success on (scode >= 0) or radio-core-failure otherwise)
    else {
        radio_sleep();
        session_pop();

        if ((scode >= 0) && (sub_mac_filter() == True)) {
            sys.mutex = SYS_MUTEX_PROCESSING;
            network_parse_bf();					// must create a new session
        }
#       if ((OT_FEATURE(SYSRF_CALLBACKS) == ENABLED) &&\
            !defined(EXTF_sys_sig_rfaterminate)  )
            sys.evt.RFA.terminate(1, scode);
#       elif defined(EXTF_sys_sig_rfaterminate)
            sys_sig_rfaterminate(1, scode);
#       endif

        sys.mutex 				= 0;
        sys.evt.RFA.event_no 	= 0;
    }
}




void sysevt_fscan() {
/// The fscan event (Foreground Scan) is used when the acquired HSS or SSS
/// period (from some offset in ISF 05 or 04 respectively) specifies a fscan as
/// opposed to an bscan.  fscan is used in anticipation of a foreground frame
/// dialog, which is a formal session.
#if (SYS_RECEIVE == ENABLED)
    m2session*  session;

#   if ((OT_FEATURE(SYSRF_CALLBACKS) == ENABLED) && !defined(EXTF_sys_sig_rfainit))
        sys.evt.RFA.init(2);
#   elif defined(EXTF_sys_sig_rfainit)
        sys_sig_rfainit(2);
#   endif
    
    /// Set up events so that the next RF event will occur when this times-out.
    /// Listening will block non-RFA events from occuring.
    sys.mutex               = SYS_MUTEX_RADIO_LISTEN;
    sys.evt.RFA.nextevent   = dll.comm.rx_timeout;
    sys.evt.RFA.event_no    = 2;
    session                 = session_top();
    
    rm2_rxinit_ff(  session->channel, 0, &rfevt_frx  );
#endif
}




void rfevt_frx(ot_int pcode, ot_int fcode) {
/// Radio Core event callback, called by the radio driver when a frame is rx'ed
/// or if there is some type of error.
    ot_int 		frx_code;
    m2session* 	session  = session_top();
    
    /// If pcode is less than zero, it is because of a listening timeout.
    /// Listening timeouts happen after unfulfilled request scanning, or after
    /// Response scanning window expires.  In certain cases, after a timeout,
    /// the session persists.  These cases are implemented below.
    if (pcode < 0) {
#   if (RF_FEATURE(RXTIMER) == ENABLED)
    	// For RF Core-based RX timer, set pcode = 0 to pre-empt kernel
    	// For MCU-based RX timer, kernel already manages timeout event
        pcode = 0;
#   endif
        sys.evt.RFA.event_no = 0;
        if (dll.comm.redundants) {
        	session->netstate = (M2_NETSTATE_REQTX | M2_NETSTATE_INIT | M2_NETFLAG_FIRSTRX);
        }
        else if (dll.comm.csmaca_params & M2_CSMACA_A2P) {
        	session->netstate ^= 0x30;	// Converts RESPRX->REQTX, REQRX->RESPTX
        }
        else {
        	session->netstate = M2_NETFLAG_SCRAP;
        }
    }
    
    // pcode: When non-negative, the number of frames remaining.
    else {
    	frx_code = 0;

        /// Handle damaged frames (bad CRC)
        /// <LI> Multiframe datastreams: mark the packet as bad, and continue </LI>
        /// <LI> Normal data packets (single frame): ignore the packet </LI>
        if (fcode != 0) {
#           if (M2_FEATURE(DATASTREAM) == ENABLED)
            if ((session->netstate & M2_NETSTATE_DSDIALOG)) 
                m2dp_mark_dsframe(session);
#           endif
            frx_code = -1;
        }
        
        /// Run subnet filtering on frames with good CRC
        else if (sub_mac_filter() == False) {
            frx_code = -4;
        }
        
        /// A complete packet has been received (errors or not).
        /// <LI> When packet is good (frx_code == 0), always process it. </LI>
        /// <LI> When request is bad or when response is any form, retry
        ///      listening until window times-out </LI>
        /// <LI> Don't return to kernel for bad frames </LI>
        /// <LI> Finish RF task after receiving a good request </LI>
        if (pcode == 0) {
        	fcode = (session->netstate & M2_NETSTATE_RESP);  // repurpose fcode
        	if (frx_code == 0) {
        		sys.mutex |= SYS_MUTEX_PROCESSING;
        	}
        	if (frx_code | fcode) {
        		pcode = frx_code;		//don't return to kernel for bad frames
        		rm2_reenter_rx(0);
        	}
        	else if (fcode == 0) {
        		sys.evt.RFA.event_no = 0;
        		radio_sleep();
        	}
        }
    }

    /// If the RF event is set to 0, it is finished and the termination
    /// callback should be used.
    if (sys.evt.RFA.event_no == 0) {
#   	if ((OT_FEATURE(SYSRF_CALLBACKS) == ENABLED) &&\
        	!defined(EXTF_sys_sig_rfaterminate)  )
        	sys.evt.RFA.terminate(2, frx_code);
#   	elif defined(EXTF_sys_sig_rfaterminate)
        	sys_sig_rfaterminate(2, frx_code);
#   	endif
    }

    /// When session restart or continuation is needed, pre-empt the kernel.
    /// The kernel clocks-down listening and contention periods, so if 
    /// restarting the session, the new listening timeout will be whatever
    /// remains from the last one.
    if (pcode == 0) {
        platform_ot_preempt();
    }
}




void sysevt_initbtx() {
#if ((M2_FEATURE(GATEWAY) == ENABLED) || \
     (M2_FEATURE(SUBCONTROLLER) == ENABLED))
    sys.evt.RFA.event_no    = 3;

#   if ((OT_FEATURE(SYSRF_CALLBACKS) == ENABLED) &&\
        !defined(EXTF_sys_sig_rfainit)  )
        sys.evt.RFA.init(3);
#   elif defined(EXTF_sys_sig_rfainit)
        sys_sig_rfainit(3);
#   endif

    rm2_txinit_bf(&rfevt_btx);
    sys.mutex               = SYS_MUTEX_RADIO_LISTEN;
#   if (RF_FEATURE(TXTIMER) == DISABLED)
    sys.evt.RFA.nextevent   = 0;    // Normal TX CSMA process
    dll.comm.tca            = dll.comm.tc;
#   else
    sys.evt.RFA.nextevent   = dll.comm.tc + sys.evt.adv_time;   // TX timeout
#   endif

#endif
}





void sysevt_initftx() {
/// Initialize the TX Engine for foreground packet transmission.  This requires
/// a CSMA-CA routine that runs prior to the data transmission.  The system 
/// layer manages TX CSMA when it is not part of the Radio Core featureset.
	sys.evt.RFA.event_no = 4;

#   if ((OT_FEATURE(SYSRF_CALLBACKS) == ENABLED) &&\
        !defined(EXTF_sys_sig_rfainit)  )
        sys.evt.RFA.init(4);
#   elif defined(EXTF_sys_sig_rfainit)
        sys_sig_rfainit(4);
#   endif
    
    ///@todo 1st argument of rm2_txinit_ff() is estimated number of frames in
    /// the packet.  for now it is hard coded to 1.
    rm2_txinit_ff(0, &rfevt_ftx);
    sys.mutex               = SYS_MUTEX_RADIO_LISTEN;
#   if (RF_FEATURE(TXTIMER) == DISABLED)
    sys.evt.RFA.nextevent   = sub_fcinit();     // Normal TX CSMA process
    dll.comm.tca            = dll.comm.tc;
#   else
    sys.evt.RFA.nextevent   = dll.comm.tc;      // TX timeout
#   endif
}









void sysevt_txcsma() {
/// Run the TX CSMA-CA routine, which requires multiple calls if radio core does
/// not have automated TX contention window handling.
#if (RF_FEATURE(TXTIMER) != ENABLED)
    ot_int csma_code = -1;
    
    /// First, check Tca to make sure we are within timing requirements
    if (dll.comm.tca >= 0) {
        csma_code = rm2_txcsma();
        
        // CSMA process continues immediately
        switch (csma_code) {
            case RM2_ERR_BADCHANNEL:
                goto sysevt_txcsma_fail;
                
            case RM2_ERR_CCAFAIL:
                sys.evt.RFA.nextevent = sub_fcloop();
                break;
            
            ///TX CSMA complete: begin data transfer                                    <BR>
            /// - Flooding handled specially (no contention, different timeout value)   <BR>
            /// - A2P must get the full packet TX'ed before the end of contention       <BR>
            /// - NA2P (normal) must start TX before end of contention
            case -1: 
            	sys.mutex               = SYS_MUTEX_RADIO_DATA;
            	sys.evt.RFA.event_no   += 2;
#               if (SYS_FLOOD == ENABLED)
                sys.evt.RFA.nextevent   = (sys.evt.RFA.event_no == 5) ? \
                                            sys.evt.adv_time : rm2_pkt_duration(txq.length);
#               else
                sys.evt.RFA.nextevent   = rm2_pkt_duration(txq.length);
#               endif
                break;
            
            default:
                sys.evt.RFA.nextevent = csma_code;
                break;
        }
    }

    else {
        sysevt_txcsma_fail:
#       if ((OT_FEATURE(SYSRF_CALLBACKS) == ENABLED) &&\
            !defined(EXTF_sys_sig_rfaterminate)  )
            sys.evt.RFA.terminate(3, csma_code);
#       elif defined(EXTF_sys_sig_rfaterminate)
            sys_sig_rfaterminate(3, csma_code);
#       endif
        session_pop();
        sys_idle();
    }

#endif
}




void rfevt_ftx(ot_int pcode, ot_int scratch) {
    m2session*  session;

    /// Non-final frame TX'ed in multiframe packet
    if (pcode == 1) {
        
        ///@todo possibly put the queue rearrangement here
    }

    /// Packet TX is done.  Handle this event and pre-empt the kernel.
    /// - Normally, go to response RX.
    /// - Allow scheduling of redundant TX on responses, or request with no response
    /// - End session if no redundant, and no listening required
    else {
        ot_u8 scrap_bit;
        sys.mutex               = 0;
        sys.evt.RFA.event_no    = 0;
        session                 = session_top();
        //scrap_bit               = (dll.comm.rx_timeout == 0) | \
        //                          ((session->netstate & M2_NETSTATE_TMASK) == M2_NETSTATE_RESPTX);
        scrap_bit               = (dll.comm.rx_timeout == 0);
        scrap_bit              |= ((session->netstate & M2_NETSTATE_RESPTX) != 0);
        dll.comm.redundants    -= 1;
        
        // Send redundant TX immediately, but only if no response window or if
        // this packet is a response.
        if (scrap_bit && dll.comm.redundants) {
            dll.comm.csmaca_params = (M2_CSMACA_NOCSMA | M2_CSMACA_MACCA);
            rm2_prep_resend();
        }
        
        // End Session if no redundant, no response window, if this itself is a
        // response, or if there is some sort of error
        else {
            scrap_bit          |= (pcode != 0);         //some sort of error
            session->netstate  |= (scrap_bit << 7);     //M2_NETFLAG_SCRAP
            session->netstate  &= ~M2_NETSTATE_TMASK;
            session->netstate  |= M2_NETSTATE_RESPRX;
        }
    
#       if ((OT_FEATURE(SYSRF_CALLBACKS) == ENABLED) && !defined(EXTF_sys_sig_rfaterminate))
            sys.evt.RFA.terminate(5, pcode);
#       elif defined(EXTF_sys_sig_rfaterminate)
            sys_sig_rfaterminate(5, pcode);
#       endif            
        
        platform_ot_preempt();
    }
}




void rfevt_btx(ot_int flcode, ot_int scratch) {
#if ((M2_FEATURE(SUBCONTROLLER) == ENABLED) || (M2_FEATURE(GATEWAY) == ENABLED))
    switch (flcode) {
        /// Flood ends & Request Begins                                     <BR>
        /// - Swap back control to foreground session (m2advp_close())      <BR>
        /// - Make sure request session is ready to go by setting to 0      <BR>
        /// - Turn CSMA-CA OFF and set TX timeout to 2 ticks
        case 0: {
            m2session* session;
#           if ((OT_FEATURE(SYSRF_CALLBACKS) == ENABLED) && !defined(EXTF_sys_sig_rfaterminate)  )
                sys.evt.RFA.terminate(4, 0);
#           elif defined(EXTF_sys_sig_rfaterminate)
                sys_sig_rfaterminate(4, 0);
#           endif
            m2advp_close();
            session                 = session_top();
            session->counter        = 0;
            sys.evt.adv_time        = 0;
            sys.evt.RFA.event_no    = 0;
            sys.mutex               = 0;
            dll.comm.tc             = 2;
            dll.comm.csmaca_params  = (M2_CSMACA_NOCSMA | M2_CSMACA_MACCA);
            dll.comm.redundants     = 1;
            platform_ot_preempt();
            break;
        }
        
        /// Flood Continues: Dump-out another flood packet                  <BR>
        /// - platform_get_gptim() must not be touched for this to work, 
        ///   which means this method only works for contiguous floods.     <BR>
        /// - End the flood if the counter is over, else maintain flood
        case 2: {
            scratch = sys.evt.adv_time - platform_get_gptim();
            ///@todo figure out how to fit this into new event manager
            
            if (scratch < rm2_pkt_duration(7)) {
                rm2_txstop_flood();
            }
            else {
                txq.front[3]    = ((ot_u8*)&scratch)[UPPER];
                txq.front[4]    = ((ot_u8*)&scratch)[LOWER];
                txq.putcursor   = &txq.front[5];
            }
            break;
        }
    
        default: {
#           if ((OT_FEATURE(SYSRF_CALLBACKS) == ENABLED) &&\
                !defined(EXTF_sys_sig_rfaterminate)  )
                sys.evt.RFA.terminate(4, flcode);
#           elif defined(EXTF_sys_sig_rfaterminate)
                sys_sig_rfaterminate(4, flcode);
#           endif
            sys_idle();
            platform_ot_preempt();
            break;
        }
    }
#endif
}





/** System Subroutines
  * ============================================================================
  */

//void sub_worevt_ctrl(wakeon_event* worevt, ot_long* eta) {
//    if (worevt->nextevent < *eta) {
//        *eta = worevt->nextevent;
//    }
//}



void sub_idlevt_ctrl(idletime_event* idlevt, ot_long* eta, ot_u8 sequence_id) {  
#if (M2_FEATURE(RTC_SCHEDULER) == ENABLED)
    if (idlevt->sched_id != 0) {
        vlFILE*     fp;
        ot_int      offset;
        ot_u16      ssmask;
        ot_u16      ssvalue;
        
        fp = ISF_open_su( ISF_ID(real_time_scheduler) );
        ///@todo assert fp
        
        // This is an arithmetic trick to convert the sequence id to
        // the proper offset (sleep, hold, beacon) of the RTC ISF
        // sleep offset = 0, hold offset = 4, beacon offset = 8
        offset  = (sequence_id-4) << 2;
        
        // Load Mask and Value (always stored as big endian)
        ssmask  = ISF_read(fp, offset);
        ssmask	= PLATFORM_ENDIAN16(ssmask);
        ssvalue = ISF_read(fp, offset+=2);
        ssvalue = PLATFORM_ENDIAN16(ssvalue);
        vl_close(fp);
        
        // Apply new mask & value to the RTC and reset the synchronized task
        platform_set_rtc_alarm(idlevt->sched_id, ssmask, ssvalue);
        idlevt->cursor      = 0;
        idlevt->nextevent   = 0;
    }
#endif
        
    if (idlevt->nextevent < *eta) {
        *eta = idlevt->nextevent;
    }
}




void sub_sys_flush() {
/// (1) Reset sessions. 
/// (2) Put System states into the right place and flush existing events. 
/// (3) Set scheduler ids and prepare idle time events
    ot_u8 accum = 0;
    
    session_init();
    dll.idle_state = sub_default_idle();
    
#   if (M2_FEATURE(ENDPOINT) == ENABLED)
#       if (M2_FEATURE(RTCSLEEP) == ENABLED)
            accum              += ((M2_SET_SLEEPSCHED & dll.netconf.active) != 0);
            sys.evt.SSS.sched_id= accum;
#       endif
        sys.evt.SSS.cursor      = 0;
        sys.evt.SSS.nextevent   = 0;
#   endif

#   if ((M2_FEATURE(ENDPOINT) == ENABLED) || \
        (M2_FEATURE(SUBCONTROLLER) == ENABLED) || \
        (M2_FEATURE(GATEWAY) == ENABLED))
#       if (M2_FEATURE(RTCHOLD) == ENABLED)
            accum              += ((M2_SET_HOLDSCHED & dll.netconf.active) != 0);
            sys.evt.HSS.sched_id= accum;
#       endif
        sys.evt.HSS.cursor      = 0;
        sys.evt.HSS.nextevent   = 0;
#   endif

#   if (M2_FEATURE(BEACONS) == ENABLED)
#       if (M2_FEATURE(RTCBEACON) == ENABLED)
            accum              += ((M2_SET_BEACONSCHED & dll.netconf.active) != 0);
            sys.evt.BTS.sched_id= accum;
#       endif
        sys.evt.BTS.cursor      = 0;
        sys.evt.BTS.event_no    = (dll.netconf.b_attempts != 0);
        sys.evt.BTS.nextevent   = 0;
#   endif

    /// Go to the appropriate idle state
    sys_idle();
}




ot_u8 sub_default_idle() {
#if ((M2_FEATURE(ENDPOINT) == ENABLED) || (M2_FEATURE(BLINKER) == ENABLED))
    ot_u16 active_setting   = dll.netconf.active & M2_SET_CLASSMASK;
    
    if (active_setting >= M2_SET_SUBCONTROLLER)       
        return M2_MACIDLE_HOLD;
    else if (active_setting & M2_SET_ENDPOINT)  
        return M2_MACIDLE_SLEEP;
    
    return M2_MACIDLE_OFF;
#else
    
    return M2_MACIDLE_HOLD;
#endif
}








/** Flow & Congestion Control Functions
  * ============================================================================
  */

ot_bool sub_mac_filter() {
/// Link Budget Filtering (LBF) is a normalized RSSI Qualifier.
/// Subnet Filtering is an numerical qualifier
    ot_bool qualifier;
    {
        // TX EIRP encoded value    = (dBm + 40) * 2
        // TX EIRP dBm              = ((encoded value) / 2) - 40
        // Link Loss                = TX EIRP dBm - Detected RX dBm
        // Link Quality Filter      = (Link Loss <= Link Loss Limit)
        ot_int linkloss;
        linkloss    = ((ot_int)((rxq.front[1] >> 1) & 0x3F) - 40) - radio_rssi(); 
        qualifier   = (ot_bool)(linkloss <= (ot_int)phymac[0].link_qual);
    }
    {
        ot_u8 fr_subnet, dsm, specifier, mask;
        
        fr_subnet   = rxq.front[2];
        dsm         = dll.netconf.subnet & 0x0F;
        mask        = fr_subnet & dsm;
        specifier   = (fr_subnet ^ dll.netconf.subnet) & 0xF0;
        fr_subnet  &= 0xF0;
        qualifier  &= (ot_bool)(((fr_subnet == 0xF0) || (specifier == 0)) && (mask == dsm));
    }
    return qualifier;
}




void sub_csma_scramble() {
/// Sort of optional: Go through the channel list and scramble the channel
/// entries randomly so that different devices aren't trying to respond on the
/// same channels at the same time.
    if (dll.comm.tx_channels > 1) {
        ot_u8 i, j, k, rot1, rot2, scratch;
        rot1 = platform_prand_u8();
        rot2 = platform_prand_u8();
        
        for (i=0; i<(dll.comm.tx_channels-1); i++) {
            j = i + ((rot1&1) != 0);
            k = (dll.comm.tx_channels-1) * ((rot2&1) != 0);
            
            scratch                 = dll.comm.tx_chanlist[i];
            dll.comm.tx_chanlist[i] = dll.comm.tx_chanlist[k];
            dll.comm.tx_chanlist[k] = scratch;
            scratch                 = dll.comm.tx_chanlist[i];
            dll.comm.tx_chanlist[i] = dll.comm.tx_chanlist[j];
            dll.comm.tx_chanlist[j] = scratch;
            
            rot1 >>= 1;
            rot2 >>= 1;
        }
    }
}




ot_uint sub_fcinit() {
/// Pick a time offset to begin the first transmission attempt, and setup
/// flow-congestion loop parameters.
    
    // Pick a slot offset: currently only RIGD and RAIND need a random slot.
    // {0,1,2,3} = {RIGD, RAIND, AIND, Default MAC CA} 
    switch ( (dll.comm.csmaca_params >> 3) & 0x03 ) {
        case 0: return sub_rigd_newslot();
                
        case 1: {
            ot_u16 random;
            random  = platform_prand_u16();
            random %= (dll.comm.tca - rm2_pkt_duration(txq.front[0]));
            return random;
        }
        
        case 2: 
        case 3: return 0;
    }
    //ignore compiler warning here (switch will always return)
}




void sub_fceval(ot_int query_score) {
/// @todo When M2QP returns zero, the query has succeeded with no priorities.  
/// Some queries have priority scores.  In these cases, higher scores may be 
/// used to affect CSMA-CA reply ordering (higher is better).  This feature is 
/// not yet implemented.
}




ot_uint sub_fcloop() {
    /// {0,1,2,3} = {RIGD, RAIND, AIND, Default MAC CA} 
    /// Default MAC CA just waits Tg before trying again
    switch ( (dll.comm.csmaca_params >> 3) & 3 ) {
        case 0: return sub_rigd_nextslot() + sub_rigd_newslot();
        case 1: 
        case 2: return sub_aind_nextslot();
        case 3: return phymac[0].tg;
    }
} //ignore compiler warning here (switch will always return)




ot_uint sub_rigd_newslot() {
/// halve tc from previous value and offset a random within that duration
    ot_u16 random;
    random          = platform_prand_u16();
    dll.comm.tc   >>= 1;
    dll.comm.tca    = dll.comm.tc;
    return          (random % dll.comm.tc);
}




ot_uint sub_rigd_nextslot() {
    ot_long wait = (dll.comm.tc - dll.comm.tca);
    return (wait < 0) ? 0 : (ot_uint)wait;
}




ot_uint sub_aind_nextslot() {
/// Works for RAIND or AIND next slot
    return rm2_pkt_duration(txq.front[0]);
}




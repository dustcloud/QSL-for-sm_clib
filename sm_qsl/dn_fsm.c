/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "dn_fsm.h"
#include "dn_ipmt.h"
#include "dn_time.h"
#include "dn_watchdog.h"
#include "dn_qsl_api.h"

//=========================== variables =======================================

typedef struct
{
	uint8_t				replyBuf[MAX_FRAME_LENGTH];
	uint8_t				notifBuf[MAX_FRAME_LENGTH];
	fsm_reply_callback	replyCb;
	fsm_timer_callback	fsmCb;
	uint32_t			fsmEventScheduled_ms;
	uint16_t			fsmDelay_ms;
	uint32_t			events;
	uint8_t				state;
} dn_fsm_vars_t;

dn_fsm_vars_t dn_fsm_vars;


//=========================== prototypes ======================================

void dn_ipmt_notif_cb(uint8_t cmdId, uint8_t subCmdId);
void dn_ipmt_reply_cb(uint8_t cmdId);
void fsm_scheduleEvent(uint16_t delay, fsm_timer_callback cb);
void fsm_cancelEvent(void);
void fsm_setReplyCallback(fsm_reply_callback cb);

//=========================== public ==========================================

void dn_fsm_init(void)
{
	// Reset local variables
	memset(&dn_fsm_vars, 0, sizeof(dn_fsm_vars));
	
	// Initialize the ipmt module
	dn_ipmt_init
			(
			dn_ipmt_notif_cb,
			dn_fsm_vars.notifBuf,
			sizeof(dn_fsm_vars.notifBuf),
			dn_ipmt_reply_cb
			);
	
	dn_fsm_vars.state = FSM_STATE_DISCONNECTED;
}

void dn_fsm_run(void)
{
	dn_err_t rc = DN_ERR_NONE;
	
	
	sleep(5);
	
	rc = dn_ipmt_getParameter_moteStatus
			(
			(dn_ipmt_getParameter_moteStatus_rpt*)dn_fsm_vars.replyBuf
			);
	printf("Requested moteStatus, rc: %u\n", rc);
	
	sleep(5);
}

//=========================== private =========================================

void fsm_scheduleEvent(uint16_t delay_ms, fsm_timer_callback cb)
{
	dn_fsm_vars.fsmEventScheduled_ms = dn_time_ms();
	dn_fsm_vars.fsmDelay_ms = delay_ms;
	dn_fsm_vars.fsmCb = cb;
}

void fsm_cancelEvent(void)
{
	dn_fsm_vars.fsmDelay_ms = 0;
	dn_fsm_vars.fsmCb = NULL;
}

void fsm_setReplyCallback(fsm_reply_callback cb)
{
	dn_fsm_vars.replyCb = cb;
}

void dn_ipmt_notif_cb(uint8_t cmdId, uint8_t subCmdId)
{
	//dn_ipmt_timeIndication_nt* notif_timeIndication;
	dn_ipmt_events_nt* notif_events;
	//dn_ipmt_receive_nt* notif_receive;
	//dn_ipmt_macRx_nt* notif_macRx;
	//dn_ipmt_txDone_nt* notif_txDone;
	//dn_ipmt_advReceived_nt* notif_advReceived;
	
	printf("Got notification: cmdId; %u, subCmdId; %u\n", cmdId, subCmdId);
	
	switch (cmdId)
	{
	case CMDID_TIMEINDICATION:
		break;
	case CMDID_EVENTS:
		notif_events = (dn_ipmt_events_nt*)dn_fsm_vars.notifBuf;
		switch(notif_events->state)
		{
		case MOTE_STATE_IDLE:
			break;
		case MOTE_STATE_OPERATIONAL:
			break;
		}
		dn_fsm_vars.events |= notif_events->events;
		
		break;
	case CMDID_RECEIVE:
		break;
	case CMDID_MACRX:
		break;
	case CMDID_TXDONE:
		break;
	case CMDID_ADVRECEIVED:
		break;
	default:
		// Unknown notification ID
		break;
	}
	
	
}

void dn_ipmt_reply_cb(uint8_t cmdId)
{
	printf("Got reply: cmdId; %u\n", cmdId);
	if (dn_fsm_vars.replyCb == NULL)
	{
		// No reply callback registered
		printf("dn_fsm: Reply callback empty\n");
		return;
	}
	dn_fsm_vars.replyCb();
}

//=========================== helpers =========================================
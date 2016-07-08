/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

#include "dn_fsm.h"
#include "dn_ipmt.h"
#include "dn_time.h"
#include "dn_watchdog.h"
#include "dn_qsl_api.h"
#include "dn_debug.h"

//=========================== variables =======================================

typedef struct
{
	// FSM
	uint32_t fsmEventScheduled_ms;
	uint16_t fsmDelay_ms;
	uint8_t state;
	// C Library API
	fsm_reply_callback replyCb;
	fsm_timer_callback fsmCb;
	uint8_t replyBuf[MAX_FRAME_LENGTH];
	uint8_t notifBuf[MAX_FRAME_LENGTH];
	// Connection
	uint8_t socketId;
	uint16_t networkId;
	uint8_t joinKey[JOIN_KEY_LEN];
	uint32_t service_ms;
	uint8_t payloadBuf[DEFAULT_PAYLOAD_SIZE_LIMIT];
	uint8_t payloadSize;
	uint8_t destIPv6[IPv6ADDR_LEN];
	uint16_t destPort;
	dn_inbox_t inbox;
} dn_fsm_vars_t;

static dn_fsm_vars_t dn_fsm_vars;


//=========================== prototypes ======================================
// FSM
static void fsm_run(void);
static void fsm_scheduleEvent(uint16_t delay, fsm_timer_callback cb);
static void fsm_cancelEvent(void);
static void fsm_setReplyCallback(fsm_reply_callback cb);
static void fsm_enterState(uint8_t newState, uint16_t spesificDelay);
static bool fsm_cmd_timeout(uint32_t cmdStart_ms, uint32_t cmdTimeout_ms);
// C Library API
static void dn_ipmt_notif_cb(uint8_t cmdId, uint8_t subCmdId);
static void dn_ipmt_reply_cb(uint8_t cmdId);
static void api_response_timeout(void); // TODO: Better prefix?
static void api_reset(void);
static void api_reset_reply(void);
static void api_disconnect(void);
static void api_disconnect_reply(void);
static void api_getMoteStatus(void);
static void api_getMoteStatus_reply(void);
static void api_openSocket(void);
static void api_openSocket_reply(void);
static void api_bindSocket(void);
static void api_bindSocket_reply(void);
static void api_setJoinKey(void);
static void api_setJoinKey_reply(void);
static void api_setNetworkId(void);
static void api_setNetworkId_reply(void);
static void api_join(void);
static void api_join_reply(void);
static void api_requestService(void);
static void api_requestService_reply(void);
static void api_getServiceInfo(void);
static void api_getServiceInfo_reply(void);
static void api_sendTo(void);
static void api_sendTo_reply(void);
// helpers
static dn_err_t checkAndSaveNetConfig(uint16_t netID, uint8_t* joinKey, uint32_t req_service_ms);
static uint8_t getPayloadLimit(uint16_t destPort);

//=========================== public ==========================================

//=== QSL API ===

bool dn_qsl_init(void)
{
	debug("QSL: Init");
	// Reset local variables
	memset(&dn_fsm_vars, 0, sizeof (dn_fsm_vars));

	// Initialize the ipmt module
	dn_ipmt_init // Should be augmented with return value to know if successful...
			(
			dn_ipmt_notif_cb,
			dn_fsm_vars.notifBuf,
			sizeof (dn_fsm_vars.notifBuf),
			dn_ipmt_reply_cb
			);
	
	fsm_enterState(FSM_STATE_DISCONNECTED, 0);	
	return TRUE;
}

bool dn_qsl_isConnected(void)
{
	debug("QSL: isConnected");
	return dn_fsm_vars.state == FSM_STATE_CONNECTED;
}

bool dn_qsl_connect(uint16_t netID, uint8_t* joinKey, uint32_t req_service_ms)
{
	uint32_t cmdStart_ms = dn_time_ms();
	dn_err_t err;
	debug("QSL: Connect");
	switch (dn_fsm_vars.state)
	{
	case FSM_STATE_NOT_INITIALIZED:
		log_warn("Can't connect; not initialized");
		// TODO: Could initialize for user?
		return FALSE;
	case FSM_STATE_DISCONNECTED:
		err = checkAndSaveNetConfig(netID, joinKey, req_service_ms);
		if (err != DN_ERR_NONE)
		{
			return FALSE;
		}
		debug("Starting connect process...");
		fsm_enterState(FSM_STATE_PRE_JOIN, 0);
		break;
	case FSM_STATE_CONNECTED:
		if ((netID > 0 && netID != dn_fsm_vars.networkId) ||
				(joinKey != NULL && memcmp(joinKey, dn_fsm_vars.joinKey, JOIN_KEY_LEN) != 0))
		{
			err = checkAndSaveNetConfig(netID, joinKey, req_service_ms);
			if (err != DN_ERR_NONE)
			{
				return FALSE;
			}
			debug("New network ID and/or join key; reconnecting...");
			fsm_enterState(FSM_STATE_RESETTING, 0);
		} else if (req_service_ms > 0 && req_service_ms != dn_fsm_vars.service_ms)
		{
			debug("New service request");
			dn_fsm_vars.service_ms = req_service_ms;
			fsm_enterState(FSM_STATE_REQ_SERVICE, 0);
		} else
		{
			debug("Already connected");
			// Nothing to do
		}
		break;
	default:
		log_err("Unexpected state");
		fsm_enterState(FSM_STATE_DISCONNECTED, 0);
		return FALSE;
	}

	// Drive FSM until connect success/failure or timeout
	while (dn_fsm_vars.state != FSM_STATE_CONNECTED
			&& dn_fsm_vars.state != FSM_STATE_DISCONNECTED
			&& !fsm_cmd_timeout(cmdStart_ms, CONNECT_TIMEOUT_S * 1000))
	{
		dn_watchdog_feed();
		fsm_run();
	}

	return dn_fsm_vars.state == FSM_STATE_CONNECTED;
}

bool dn_qsl_send(uint8_t* payload, uint8_t payloadSize_B, uint16_t destPort)
{
	uint32_t cmdStart_ms = dn_time_ms();
	uint8_t maxPayloadSize;
	debug("QSL: Send");
	switch (dn_fsm_vars.state)
	{
	case FSM_STATE_CONNECTED:
		maxPayloadSize = getPayloadLimit(destPort);

		if (payloadSize_B > maxPayloadSize)
		{
			log_warn("Payload size (%u) exceeds limit (%u)", payloadSize_B, maxPayloadSize);
			return FALSE;
		}
		// Store outbound payload and parameters
		memcpy(dn_fsm_vars.payloadBuf, payload, payloadSize_B);
		dn_fsm_vars.payloadSize = payloadSize_B;
		memcpy(dn_fsm_vars.destIPv6, DEST_IP, IPv6ADDR_LEN);
		dn_fsm_vars.destPort = (destPort > 0) ? destPort : DEFAULT_DEST_PORT;
		// Start send process
		fsm_enterState(FSM_STATE_SENDING, 0);
		break;
	default:
		log_warn("Can't send; not connected");
		return FALSE;
	}

	// Drive FSM until send success/failure or timeout
	while (dn_fsm_vars.state == FSM_STATE_SENDING
			&& !fsm_cmd_timeout(cmdStart_ms, SEND_TIMEOUT_MS))
	{
		dn_watchdog_feed();
		fsm_run();
	}

	// Catch send failure
	if (dn_fsm_vars.state == FSM_STATE_SEND_FAILED)
	{
		debug("Send failed");
		fsm_enterState(FSM_STATE_CONNECTED, 0);
		return FALSE;
	}

	return dn_fsm_vars.state == FSM_STATE_CONNECTED;
}

uint8_t dn_qsl_read(uint8_t* readBuffer)
{
	uint8_t bytesRead = 0;
	debug("QSL: Read");
	if (dn_fsm_vars.inbox.unreadPackets > 0)
	{
		// Pop payload at head of inbox
		memcpy
				(
				readBuffer,
				dn_fsm_vars.inbox.pktBuf[dn_fsm_vars.inbox.head],
				dn_fsm_vars.inbox.pktSize[dn_fsm_vars.inbox.head]
				);
		bytesRead = dn_fsm_vars.inbox.pktSize[dn_fsm_vars.inbox.head];
		dn_fsm_vars.inbox.head = (dn_fsm_vars.inbox.head + 1) % INBOX_SIZE;
		dn_fsm_vars.inbox.unreadPackets--;
		debug("Read %u bytes from inbox", bytesRead);
	} else
	{
		debug("Inbox empty");
	}
	return bytesRead;
}

//=========================== private =========================================

//=== FSM ===

static void fsm_run(void)
{
	if (dn_fsm_vars.fsmDelay_ms > 0 && (dn_time_ms() - dn_fsm_vars.fsmEventScheduled_ms > dn_fsm_vars.fsmDelay_ms))
	{
		// Scheduled event is due
		dn_fsm_vars.fsmDelay_ms = 0;
		if (dn_fsm_vars.fsmCb != NULL)
		{
			dn_fsm_vars.fsmCb();
		}
	} else
	{
		// Sleep to save CPU power
		dn_sleep_ms(FSM_RUN_INTERVAL_MS);
	}
}

static void fsm_scheduleEvent(uint16_t delay_ms, fsm_timer_callback cb)
{
	dn_fsm_vars.fsmEventScheduled_ms = dn_time_ms(); // TODO: Move to each cmd?
	dn_fsm_vars.fsmDelay_ms = delay_ms;
	dn_fsm_vars.fsmCb = cb;
}

static void fsm_cancelEvent(void)
{
	dn_fsm_vars.fsmDelay_ms = 0;
	dn_fsm_vars.fsmCb = NULL;
}

static void fsm_setReplyCallback(fsm_reply_callback cb)
{
	dn_fsm_vars.replyCb = cb;
}

static void fsm_enterState(uint8_t newState, uint16_t spesificDelay)
{
	static uint32_t lastTransition = 0;
	uint32_t now = dn_time_ms();
	uint16_t delay = CMD_PERIOD_MS;

	if (lastTransition == 0)
		lastTransition = dn_time_ms();
	if (spesificDelay > 0)
		delay = spesificDelay;

	// Schedule default events for transition into states
	switch (newState)
	{
	case FSM_STATE_PRE_JOIN:
		fsm_scheduleEvent(delay, api_getMoteStatus);
		break;
	case FSM_STATE_JOINING:
		fsm_scheduleEvent(CMD_PERIOD_MS, api_join);
		break;
	case FSM_STATE_REQ_SERVICE:
		fsm_scheduleEvent(delay, api_requestService);
		break;
	case FSM_STATE_RESETTING:
		fsm_scheduleEvent(delay, api_reset); // Faster
		//fsm_scheduleEvent(delay, api_disconnect); // More graceful
		break;
	case FSM_STATE_SENDING:
		api_sendTo();
		break;
	case FSM_STATE_SEND_FAILED:
	case FSM_STATE_DISCONNECTED:
	case FSM_STATE_CONNECTED:
		// These states have no default entry events
		break;
	default:
		log_warn("Attempt at entering unexpected state %u", newState);
		return;
	}

	debug("FSM state transition: %#x --> %#x (%u ms)",
			dn_fsm_vars.state, newState, now - lastTransition);
	lastTransition = now;
	dn_fsm_vars.state = newState;
}

static bool fsm_cmd_timeout(uint32_t cmdStart_ms, uint32_t cmdTimeout_ms)
{
	bool timeout = (dn_time_ms() - cmdStart_ms) > cmdTimeout_ms;
	if (timeout)
	{
		// Cancel any ongoing transmission or scheduled event and reset reply cb
		dn_ipmt_cancelTx();
		dn_fsm_vars.replyCb = NULL;
		fsm_cancelEvent();

		// Default timeout state is different while connecting vs sending
		switch (dn_fsm_vars.state)
		{
		case FSM_STATE_PRE_JOIN:
		case FSM_STATE_JOINING:
		case FSM_STATE_REQ_SERVICE:
		case FSM_STATE_RESETTING:
			debug("Connect timeout");
			fsm_enterState(FSM_STATE_DISCONNECTED, 0);
			break;
		case FSM_STATE_SENDING:
			debug("Send timeout");
			fsm_enterState(FSM_STATE_SEND_FAILED, 0);
			break;
		default:
			log_err("Command timeout in unexpected state: %#x", dn_fsm_vars.state);
			break;
		}
	}
	return timeout;
}

//=== C Library API ===

static void dn_ipmt_notif_cb(uint8_t cmdId, uint8_t subCmdId)
{
	//dn_ipmt_timeIndication_nt* notif_timeIndication;
	dn_ipmt_events_nt* notif_events;
	dn_ipmt_receive_nt* notif_receive;
	//dn_ipmt_macRx_nt* notif_macRx;
	//dn_ipmt_txDone_nt* notif_txDone;
	//dn_ipmt_advReceived_nt* notif_advReceived;

	debug("Got notification: cmdId; %#x (%u), subCmdId; %#x (%u)",
			cmdId, cmdId, subCmdId, subCmdId);

	switch (cmdId)
	{
	case CMDID_TIMEINDICATION:
		// Not implemented
		break;
	case CMDID_EVENTS:
		notif_events = (dn_ipmt_events_nt*)dn_fsm_vars.notifBuf;
		debug("State: %#x | Events: %#x", notif_events->state, notif_events->events);

		// Check if in fsm states where we expect certain mote events
		switch (dn_fsm_vars.state)
		{
		case FSM_STATE_JOINING:
			if (notif_events->events & MOTE_EVENT_MASK_OPERATIONAL)
			{
				// Join complete
				if (dn_fsm_vars.service_ms > 0)
				{
					fsm_enterState(FSM_STATE_REQ_SERVICE, 0);
				} else
				{
					fsm_enterState(FSM_STATE_CONNECTED, 0);
				}
				return;
			}
		case FSM_STATE_REQ_SERVICE:
			if (notif_events->events & MOTE_EVENT_MASK_SVC_CHANGE)
			{
				// Service request complete; check what we were granted
				fsm_scheduleEvent(CMD_PERIOD_MS, api_getServiceInfo);
				return;
			}
		}

		// Check if reported mote state should trigger fsm state transition
		switch (notif_events->state)
		{
		case MOTE_STATE_IDLE:
			switch (dn_fsm_vars.state)
			{
			case FSM_STATE_PRE_JOIN:
			case FSM_STATE_JOINING:
			case FSM_STATE_REQ_SERVICE:
			case FSM_STATE_RESETTING:
				// Restart during connect; retry
				fsm_enterState(FSM_STATE_PRE_JOIN, 0);
				break;
			case FSM_STATE_CONNECTED:
			case FSM_STATE_SENDING:
				// Disconnect/reset; set state accordingly
				fsm_enterState(FSM_STATE_DISCONNECTED, 0);
				break;
			}
			break;
		case MOTE_STATE_OPERATIONAL:
			switch (dn_fsm_vars.state)
			{
			case FSM_STATE_PRE_JOIN:
				/*
				 Early (and unexpected) operational (connected to network)
				 during connect; reset and retry
				 */
				fsm_enterState(FSM_STATE_RESETTING, 0);
				break;
			}
			break;
		}
		break;
	case CMDID_RECEIVE:
		notif_receive = (dn_ipmt_receive_nt*)dn_fsm_vars.notifBuf;
		debug("Received downstream data");

		if (dn_fsm_vars.inbox.unreadPackets < INBOX_SIZE)
		{
			// Push payload at tail of inbox
			memcpy
					(
					dn_fsm_vars.inbox.pktBuf[dn_fsm_vars.inbox.tail],
					notif_receive->payload,
					notif_receive->payloadLen
					);
			dn_fsm_vars.inbox.pktSize[dn_fsm_vars.inbox.tail] = notif_receive->payloadLen;
			dn_fsm_vars.inbox.tail = (dn_fsm_vars.inbox.tail + 1) % INBOX_SIZE;
			dn_fsm_vars.inbox.unreadPackets++;
		} else
		{
			log_warn("Inbox overflow");
		}
		break;
	case CMDID_MACRX:
		// Not implemented
		break;
	case CMDID_TXDONE:
		// Not implemented
		break;
	case CMDID_ADVRECEIVED:
		// Not implemented (TODO: Promiscuous connect?)
		break;
	default:
		log_warn("Unknown notification ID");
		break;
	}
}

static void dn_ipmt_reply_cb(uint8_t cmdId)
{
	debug("Got reply: cmdId; %#x (%u)", cmdId, cmdId);
	if (dn_fsm_vars.replyCb == NULL)
	{
		debug("Reply callback empty");
		return;
	}
	dn_fsm_vars.replyCb();
}

static void api_response_timeout(void)
{
	debug("Response timeout");

	// Cancel any ongoing transmission and reset reply cb
	dn_ipmt_cancelTx();
	dn_fsm_vars.replyCb = NULL;

	switch (dn_fsm_vars.state)
	{
	case FSM_STATE_PRE_JOIN:
	case FSM_STATE_JOINING:
	case FSM_STATE_REQ_SERVICE:
	case FSM_STATE_RESETTING:
		// Response timeout during connect; retry
		fsm_enterState(FSM_STATE_PRE_JOIN, 0);
		break;
	case FSM_STATE_SENDING:
		// Response timeout during send; fail (TODO: Try again instead?)
		fsm_enterState(FSM_STATE_SEND_FAILED, 0);
		break;
	default:
		log_err("Response timeout in unexpected state: %#x", dn_fsm_vars.state);
		break;
	}
}

static void api_reset(void)
{
	debug("Reset");
	// Arm reply callback
	fsm_setReplyCallback(api_reset_reply);

	// Issue mote API command
	dn_ipmt_reset
			(
			(dn_ipmt_reset_rpt*)dn_fsm_vars.replyBuf
			);

	// Schedule timeout for reply
	fsm_scheduleEvent(SERIAL_RESPONSE_TIMEOUT_MS, api_response_timeout);
}

static void api_reset_reply(void)
{
	dn_ipmt_reset_rpt* reply;
	debug("Reset reply");

	// Cancel reply timeout
	fsm_cancelEvent();

	// Parse reply
	reply = (dn_ipmt_reset_rpt*)dn_fsm_vars.replyBuf;

	// Choose next event or state transition
	switch (reply->RC)
	{
	case RC_OK:
		debug("Mote soft-reset initiated");
		// Will wait for notification of reboot
		break;
	default:
		log_warn("Unexpected response code: %#x", reply->RC);
		fsm_enterState(FSM_STATE_DISCONNECTED, 0);
		break;
	}
}

static void api_disconnect(void)
{
	debug("Disconnect");

	// Arm reply callback
	fsm_setReplyCallback(api_disconnect_reply);

	// Issue mote API command
	dn_ipmt_disconnect
			(
			(dn_ipmt_disconnect_rpt*)dn_fsm_vars.replyBuf
			);

	// Schedule timeout for reply
	fsm_scheduleEvent(SERIAL_RESPONSE_TIMEOUT_MS, api_response_timeout);
}

static void api_disconnect_reply(void)
{
	dn_ipmt_disconnect_rpt* reply;
	debug("Disconnect reply");

	// Cancel reply timeout
	fsm_cancelEvent();

	// Parse reply
	reply = (dn_ipmt_disconnect_rpt*)dn_fsm_vars.replyBuf;

	// Choose next event or state transition
	switch (reply->RC)
	{
	case RC_OK:
		debug("Mote disconnect initiated");
		// Will wait for notification of reboot
		break;
	default:
		log_warn("Unexpected response code: %#x", reply->RC);
		fsm_scheduleEvent(CMD_PERIOD_MS, api_reset);
		break;
	}
}

static void api_getMoteStatus(void)
{
	debug("Mote status");

	// Arm reply callback
	fsm_setReplyCallback(api_getMoteStatus_reply);

	// Issue mote API command
	dn_ipmt_getParameter_moteStatus
			(
			(dn_ipmt_getParameter_moteStatus_rpt*)dn_fsm_vars.replyBuf
			);

	// Schedule timeout for reply
	fsm_scheduleEvent(SERIAL_RESPONSE_TIMEOUT_MS, api_response_timeout);
}

static void api_getMoteStatus_reply(void)
{
	dn_ipmt_getParameter_moteStatus_rpt* reply;
	debug("Mote status reply");

	// Cancel reply timeout
	fsm_cancelEvent();

	// Parse reply
	reply = (dn_ipmt_getParameter_moteStatus_rpt*)dn_fsm_vars.replyBuf;
	debug("State: %#x", reply->state);

	// Choose next event or state transition
	switch (reply->state)
	{
	case MOTE_STATE_IDLE:
		fsm_scheduleEvent(CMD_PERIOD_MS, api_openSocket);
		break;
	case MOTE_STATE_OPERATIONAL:
		fsm_enterState(FSM_STATE_RESETTING, 0);
		break;
	default:
		fsm_enterState(FSM_STATE_RESETTING, 0);
		break;
	}
}

static void api_openSocket(void)
{
	debug("Open socket");

	// Arm reply callback
	fsm_setReplyCallback(api_openSocket_reply);

	// Issue mote API command
	dn_ipmt_openSocket
			(
			PROTOCOL_TYPE_UDP,
			(dn_ipmt_openSocket_rpt*)dn_fsm_vars.replyBuf
			);

	// Schedule timeout for reply
	fsm_scheduleEvent(SERIAL_RESPONSE_TIMEOUT_MS, api_response_timeout);
}

static void api_openSocket_reply(void)
{
	dn_ipmt_openSocket_rpt* reply;
	debug("Open socket reply");

	// Cancel reply timeout
	fsm_cancelEvent();

	// Parse reply
	reply = (dn_ipmt_openSocket_rpt*)dn_fsm_vars.replyBuf;

	// Choose next event or state transition
	switch (reply->RC)
	{
	case RC_OK:
		debug("Socket %d opened successfully", reply->socketId);
		dn_fsm_vars.socketId = reply->socketId;
		fsm_scheduleEvent(CMD_PERIOD_MS, api_bindSocket);
		break;
	case RC_NO_RESOURCES:
		debug("Couldn't create socket due to resource availability");
		// Own state for disconnecting?
		fsm_enterState(FSM_STATE_RESETTING, 0);
		break;
	default:
		log_warn("Unexpected response code: %#x", reply->RC);
		fsm_enterState(FSM_STATE_DISCONNECTED, 0);
		break;
	}
}

static void api_bindSocket(void)
{
	debug("Bind socket");

	// Arm reply callback
	fsm_setReplyCallback(api_bindSocket_reply);

	// Issue mote API command
	dn_ipmt_bindSocket
			(
			dn_fsm_vars.socketId,
			INBOX_PORT,
			(dn_ipmt_bindSocket_rpt*)dn_fsm_vars.replyBuf
			);

	// Schedule timeout for reply
	fsm_scheduleEvent(SERIAL_RESPONSE_TIMEOUT_MS, api_response_timeout);
}

static void api_bindSocket_reply(void)
{
	dn_ipmt_bindSocket_rpt* reply;
	debug("Bind socket reply");

	// Cancel reply timeout
	fsm_cancelEvent();

	// Parse reply
	reply = (dn_ipmt_bindSocket_rpt*)dn_fsm_vars.replyBuf;

	// Choose next event or state transition
	switch (reply->RC)
	{
	case RC_OK:
		debug("Socket bound successfully");
		fsm_scheduleEvent(CMD_PERIOD_MS, api_setJoinKey);
		break;
	case RC_BUSY:
		debug("Port already bound");
		// Own state for disconnect?
		fsm_enterState(FSM_STATE_RESETTING, 0);
		break;
	case RC_NOT_FOUND:
		debug("Invalid socket ID");
		fsm_enterState(FSM_STATE_DISCONNECTED, 0);
		break;
	default:
		log_warn("Unexpected response code: %#x", reply->RC);
		fsm_enterState(FSM_STATE_DISCONNECTED, 0);
		break;
	}
}

static void api_setJoinKey(void)
{
	debug("Set join key");

	// Arm reply callback
	fsm_setReplyCallback(api_setJoinKey_reply);

	// Issue mote API command
	dn_ipmt_setParameter_joinKey
			(
			dn_fsm_vars.joinKey,
			(dn_ipmt_setParameter_joinKey_rpt*)dn_fsm_vars.replyBuf
			);

	// Schedule timeout for reply
	fsm_scheduleEvent(SERIAL_RESPONSE_TIMEOUT_MS, api_response_timeout);
}

static void api_setJoinKey_reply(void)
{
	dn_ipmt_setParameter_joinKey_rpt* reply;
	debug("Set join key reply");

	// Cancel reply timeout
	fsm_cancelEvent();

	// Parse reply
	reply = (dn_ipmt_setParameter_joinKey_rpt*)dn_fsm_vars.replyBuf;

	// Choose next event or state transition
	switch (reply->RC)
	{
	case RC_OK:
		debug("Join key set");
		fsm_scheduleEvent(CMD_PERIOD_MS, api_setNetworkId);
		break;
	case RC_WRITE_FAIL:
		debug("Could not write the key to storage");
		fsm_enterState(FSM_STATE_DISCONNECTED, 0);
		break;
	default:
		log_warn("Unexpected response code: %#x", reply->RC);
		fsm_enterState(FSM_STATE_DISCONNECTED, 0);
		break;
	}
}

static void api_setNetworkId(void)
{
	debug("Set network ID");

	// Arm reply callback
	fsm_setReplyCallback(api_setNetworkId_reply);

	// Issue mote API command
	dn_ipmt_setParameter_networkId
			(
			dn_fsm_vars.networkId,
			(dn_ipmt_setParameter_networkId_rpt*)dn_fsm_vars.replyBuf
			);

	// Schedule timeout for reply
	fsm_scheduleEvent(SERIAL_RESPONSE_TIMEOUT_MS, api_response_timeout);
}

static void api_setNetworkId_reply(void)
{
	dn_ipmt_setParameter_networkId_rpt* reply;
	debug("Set network ID reply");

	// Cancel reply timeout
	fsm_cancelEvent();

	// Parse reply
	reply = (dn_ipmt_setParameter_networkId_rpt*)dn_fsm_vars.replyBuf;

	// Choose next event or state transition
	switch (reply->RC)
	{
	case RC_OK:
		debug("Network ID set");
		fsm_enterState(FSM_STATE_JOINING, 0);
		break;
	case RC_WRITE_FAIL:
		debug("Could not write the network ID to storage");
		fsm_enterState(FSM_STATE_DISCONNECTED, 0);
		break;
	default:
		log_warn("Unexpected response code: %#x", reply->RC);
		fsm_enterState(FSM_STATE_DISCONNECTED, 0);
		break;
	}
}

static void api_join(void)
{
	debug("Join");

	// Arm reply callback
	fsm_setReplyCallback(api_join_reply);

	// Issue mote API command
	dn_ipmt_join
			(
			(dn_ipmt_join_rpt*)dn_fsm_vars.replyBuf
			);

	// Schedule timeout for reply
	fsm_scheduleEvent(SERIAL_RESPONSE_TIMEOUT_MS, api_response_timeout);
}

static void api_join_reply(void)
{
	dn_ipmt_join_rpt* reply;
	debug("Join reply");

	// Cancel reply timeout
	fsm_cancelEvent();

	// Parse reply
	reply = (dn_ipmt_join_rpt*)dn_fsm_vars.replyBuf;

	// Choose next event or state transition
	switch (reply->RC)
	{
	case RC_OK:
		debug("Join operation started");
		// Will wait for join complete notification (operational event)
		break;
	case RC_INVALID_STATE:
		debug("The mote is in an invalid state to start join operation");
		fsm_enterState(FSM_STATE_RESETTING, 0);
		break;
	case RC_INCOMPLETE_JOIN_INFO:
		debug("Incomplete configuration to start joining");
		fsm_enterState(FSM_STATE_RESETTING, 0);
		break;
	default:
		log_warn("Unexpected response code: %#x", reply->RC);
		fsm_enterState(FSM_STATE_DISCONNECTED, 0);
		break;
	}
}

static void api_requestService(void)
{
	debug("Request service");

	// Arm reply callback
	fsm_setReplyCallback(api_requestService_reply);

	// Issue mote API command
	dn_ipmt_requestService
			(
			SERVICE_ADDRESS,
			SERVICE_TYPE_BW,
			dn_fsm_vars.service_ms,
			(dn_ipmt_requestService_rpt*)dn_fsm_vars.replyBuf
			);

	// Schedule timeout for reply
	fsm_scheduleEvent(SERIAL_RESPONSE_TIMEOUT_MS, api_response_timeout);
}

static void api_requestService_reply(void)
{
	dn_ipmt_requestService_rpt* reply;
	debug("Request service reply");

	// Cancel reply timeout
	fsm_cancelEvent();

	// Parse reply
	reply = (dn_ipmt_requestService_rpt*)dn_fsm_vars.replyBuf;

	// Choose next event or state transition
	switch (reply->RC)
	{
	case RC_OK:
		debug("Service request accepted");
		// Will wait for svcChanged notification
		break;
	default:
		log_warn("Unexpected response code: %#x", reply->RC);
		fsm_enterState(FSM_STATE_DISCONNECTED, 0);
		break;
	}
}

static void api_getServiceInfo(void)
{
	debug("Get service info");

	// Arm reply callback
	fsm_setReplyCallback(api_getServiceInfo_reply);

	// Issue mote API command
	dn_ipmt_getServiceInfo
			(
			SERVICE_ADDRESS,
			SERVICE_TYPE_BW,
			(dn_ipmt_getServiceInfo_rpt*)dn_fsm_vars.replyBuf
			);

	// Schedule timeout for reply
	fsm_scheduleEvent(SERIAL_RESPONSE_TIMEOUT_MS, api_response_timeout);
}

static void api_getServiceInfo_reply(void)
{
	dn_ipmt_getServiceInfo_rpt* reply;
	debug("Get service info reply");

	// Cancel reply timeout
	fsm_cancelEvent();

	// Parse reply
	reply = (dn_ipmt_getServiceInfo_rpt*)dn_fsm_vars.replyBuf;

	// Choose next event or state transition
	switch (reply->RC)
	{
	case RC_OK:
		if (reply->state == SERVICE_STATE_COMPLETED)
		{
			if (reply->value <= dn_fsm_vars.service_ms)
			{
				log_info("Granted service of %u ms (requested %u ms)", reply->value, dn_fsm_vars.service_ms);
			} else
			{
				log_warn("Only granted service of %u ms (requested %u ms)", reply->value, dn_fsm_vars.service_ms);
				// TODO: Should maybe fail?
			}
			fsm_enterState(FSM_STATE_CONNECTED, 0);
		} else
		{
			debug("Service request still pending");
			fsm_scheduleEvent(CMD_PERIOD_MS, api_getServiceInfo);
		}
		break;
	default:
		log_warn("Unexpected response code: %#x", reply->RC);
		fsm_enterState(FSM_STATE_DISCONNECTED, 0);
		break;
	}
}

static void api_sendTo(void)
{
	dn_err_t err;
	debug("Send");

	// Arm reply callback
	fsm_setReplyCallback(api_sendTo_reply);

	// Issue mote API command
	err = dn_ipmt_sendTo
			(
			dn_fsm_vars.socketId,
			dn_fsm_vars.destIPv6,
			dn_fsm_vars.destPort,
			SERVICE_TYPE_BW,
			PACKET_PRIORITY_MEDIUM,
			PACKET_ID_NO_NOTIF,
			dn_fsm_vars.payloadBuf,
			dn_fsm_vars.payloadSize,
			(dn_ipmt_sendTo_rpt*)dn_fsm_vars.replyBuf
			);
	if (err != DN_ERR_NONE)
	{
		debug("Send error: %u", err);
	}

	// Schedule timeout for reply
	fsm_scheduleEvent(SERIAL_RESPONSE_TIMEOUT_MS, api_response_timeout);
}

static void api_sendTo_reply(void)
{
	dn_ipmt_sendTo_rpt* reply;
	debug("Send reply");

	// Cancel reply timeout
	fsm_cancelEvent();

	// Parse reply
	reply = (dn_ipmt_sendTo_rpt*)dn_fsm_vars.replyBuf;

	// Choose next event or state transition
	switch (reply->RC)
	{
	case RC_OK:
		debug("Packet was queued up for transmission");
		fsm_enterState(FSM_STATE_CONNECTED, 0);
		break;
	case RC_NO_RESOURCES:
		debug("No queue space to accept the packet");
		fsm_enterState(FSM_STATE_SEND_FAILED, 0);
		break;
	default:
		log_warn("Unexpected response code: %#x", reply->RC);
		fsm_enterState(FSM_STATE_SEND_FAILED, 0);
		break;
	}
}

//=========================== helpers =========================================

static dn_err_t checkAndSaveNetConfig(uint16_t netID, uint8_t* joinKey, uint32_t req_service_ms)
{
	if (netID == 0)
	{
		debug("No network ID given; using default");
		dn_fsm_vars.networkId = DEFAULT_NET_ID;
	} else if (netID == 0xFFFF)
	{
		log_err("Invalid network ID: %#x (%u)", netID, netID);
		return DN_ERR_MALFORMED;
	} else
	{
		dn_fsm_vars.networkId = netID;
	}

	if (joinKey == NULL)
	{
		debug("No join key given; using default");
		memcpy(dn_fsm_vars.joinKey, DEFAULT_JOIN_KEY, JOIN_KEY_LEN);
	} else
	{
		memcpy(dn_fsm_vars.joinKey, joinKey, JOIN_KEY_LEN);
	}

	dn_fsm_vars.service_ms = req_service_ms;

	return DN_ERR_NONE;
}

static uint8_t getPayloadLimit(uint16_t destPort)
{
	bool destIsF0Bx = (destPort >= WELL_KNOWN_PORT_1 && destPort <= WELL_KNOWN_PORT_8);
	bool srcIsF0Bx = (INBOX_PORT >= WELL_KNOWN_PORT_1 && INBOX_PORT <= WELL_KNOWN_PORT_8);
	int8_t destIsMng = memcmp(DEST_IP, DEFAULT_DEST_IP, IPv6ADDR_LEN);

	if (destIsMng == 0)
	{
		if (destIsF0Bx && srcIsF0Bx)
			return PAYLOAD_SIZE_LIMIT_MNG_HIGH;
		else if (destIsF0Bx || srcIsF0Bx)
			return PAYLOAD_SIZE_LIMIT_MNG_MED;
		else
			return PAYLOAD_SIZE_LIMIT_MNG_LOW;
	} else
	{
		if (destIsF0Bx && srcIsF0Bx)
			return PAYLOAD_SIZE_LIMIT_IP_HIGH;
		else if (destIsF0Bx || srcIsF0Bx)
			return PAYLOAD_SIZE_LIMIT_IP_MED;
		else
			return PAYLOAD_SIZE_LIMIT_IP_LOW;
	}
}

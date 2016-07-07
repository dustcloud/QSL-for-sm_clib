/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

#include <unistd.h>		//Used for UART
#include <fcntl.h>		//Used for UART
#include <termios.h>	//Used for UART
#include <stdlib.h>
#include <pthread.h>

#include "dn_uart.h"
#include "dn_ipmt.h"
#include "dn_debug.h"

//=========================== variables =======================================

typedef struct {
	dn_uart_rxByte_cbt		ipmt_uart_rxByte_cb;
	int32_t					uart_fd;
	pthread_t				read_daemon;
} dn_uart_vars_t;

static dn_uart_vars_t dn_uart_vars;


//=========================== prototypes ======================================

static void* dn_uart_read_daemon(void* arg);


//=========================== public ==========================================

void dn_uart_init(dn_uart_rxByte_cbt rxByte_cb)
{
	char *portname = "/dev/ttyUSB0";
	struct termios options;
	int32_t rc;
	
	dn_uart_vars.ipmt_uart_rxByte_cb = rxByte_cb;
	
	dn_uart_vars.uart_fd = -1;
	dn_uart_vars.uart_fd = open(portname, O_RDWR | O_NOCTTY);
	if (dn_uart_vars.uart_fd == -1)
	{
		log_err("Unable to open UART %s", portname);
	} else
	{
		debug("UART opened");
	}

	tcgetattr(dn_uart_vars.uart_fd, &options);
	options.c_cflag = B115200 | CS8 | CLOCAL | CREAD;
	options.c_iflag = IGNPAR;
	options.c_oflag = 0;
	options.c_lflag = 0;
	tcflush(dn_uart_vars.uart_fd, TCIFLUSH);
	tcsetattr(dn_uart_vars.uart_fd, TCSANOW, &options);
	
	rc = pthread_create(&dn_uart_vars.read_daemon, NULL, dn_uart_read_daemon, NULL);
	if (rc != 0)
	{
		log_err("Failed to start read daemon");
	}
}

void dn_uart_txByte(uint8_t byte)
{
	int32_t rc;
	if (dn_uart_vars.uart_fd == -1)
	{
		log_err("UART not initialized (tx)");
		return;
	}
	
	rc = write(dn_uart_vars.uart_fd, &byte, 1);
	if (rc < 0)
	{
		// Error
	} else if (rc == 0)
	{
		// Nothing was sent
	} else
	{
		//debug("dn_uart: Sent a byte");
	}
}

void dn_uart_txFlush(void)
{
	// Nothing to do since POSIX driver is byte-oriented
}


//=========================== private =========================================

static void* dn_uart_read_daemon(void* arg)
{
	struct timeval timeout;
	int32_t rc;
	fd_set set;
	uint8_t rxBuff[MAX_FRAME_LENGTH];
	int8_t rxBytes = 0;
	uint8_t n = 0;
	
	if (dn_uart_vars.uart_fd == -1)
	{
		log_err("UART not initialized (rx)");
		return NULL;
	}
	debug("Read daemon started");	
	
	while(TRUE)
	{
		// Add socket to set
		FD_ZERO(&set);
		FD_SET(dn_uart_vars.uart_fd, &set);
		// Set select timeout to 0.5 seconds
		timeout.tv_sec = 0;
		timeout.tv_usec = 500000;
		rc = select(dn_uart_vars.uart_fd + 1, &set, NULL, NULL, &timeout);
		if (rc < 0)
		{
			// Error
		} else if (rc == 0)
		{
			// Timeout
		} else
		{
			// There are Bytes ready to be read
			rxBytes = read(dn_uart_vars.uart_fd, rxBuff, MAX_FRAME_LENGTH);
			if (rxBytes > 0)
			{
				debug("Received %d bytes", rxBytes);
				for (n = 0; n < rxBytes; n++)
				{
					dn_uart_vars.ipmt_uart_rxByte_cb(rxBuff[n]);
				}
			}
		}
	}
	
	
}


//=========================== helpers =========================================

//=========================== interrupt handlers ==============================
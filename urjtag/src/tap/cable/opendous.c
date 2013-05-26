/*opendous_
 * $Id: opendous.c,v 1.8 2003/08/19 08:42:20 telka Exp $
 *
 * Opendous cable driver
 *
 * Copyright (C) 2009 Vladimir Fonov
 *
 * Based on J-Link cable driver by K. Waschk 
 *
 * Large portions of code were taken from the OpenOCD driver written by
 * Juergen Stuber, which in turn was based on Dominic Rath's and Benedikt
 * Sauter's usbprog.c. Therefore most of this code is actually
 *
 * Copyright (C) 2007 Juergen Stuber
 *
 * Modified to work in UrJTAG by K. Waschk in 2008.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 */


#include "sysdep.h"

#include "generic.h"
#include "generic_usbconn.h"

#include "urjtag/usbconn.h"
#include "usbconn/libusb.h"

/* ---------------------------------------------------------------------- */


#include "cable.h"
#include "chain.h"

#include "jtag.h"

#include <string.h>

#define INFO(...)   printf(__VA_ARGS__)
#define ERROR(...)  printf(__VA_ARGS__)
#define DEBUG(...)

#define OPENDOUS_WRITE_ENDPOINT 0x02
#define OPENDOUS_READ_ENDPOINT  0x81

#define OPENDOUS_USB_TIMEOUT     1000

#define OPENDOUS_IN_BUFFER_SIZE  64
#define OPENDOUS_OUT_BUFFER_SIZE 64

#define OPENDOUS_TAP_BUFFER_SIZE (63)
#define OPENDOUS_SCHEDULE_BUFFER_SIZE 1024


#undef DEBUG_TRANSFER_STATS

//OPENDOUS JTAG COMNMANDS
#define JTAG_CMD_TAP_OUTPUT     0x0
#define JTAG_CMD_SET_TRST       0x1
#define JTAG_CMD_SET_SRST       0x2
#define JTAG_CMD_READ_INPUT     0x3
#define JTAG_CMD_TAP_OUTPUT_EMU 0x4
#define JTAG_CMD_SET_DELAY      0x5

#define OPENDOUS_MAX_SPEED 4000

typedef struct
{
  /* Global USB buffers */
  unsigned char usb_in_buffer[OPENDOUS_IN_BUFFER_SIZE];
  unsigned char usb_out_buffer[OPENDOUS_OUT_BUFFER_SIZE];

  int tap_length;
  uint8_t tms_buffer[OPENDOUS_TAP_BUFFER_SIZE];
  int last_tdo;
  
  uint8_t schedule_tap[OPENDOUS_SCHEDULE_BUFFER_SIZE];
  uint8_t schedule_tdo[OPENDOUS_SCHEDULE_BUFFER_SIZE];
  int schedule_tap_length;
  
}
opendous_usbconn_data_t;

#ifdef DEBUG_TRANSFER_STATS
FILE *debug_log=0;
#endif //DEBUG_TRANSFER_STATS


/* Queue command functions */
static void urj_tap_cable_opendous_reset (urj_usbconn_libusb_param_t *params,
                                          int trst, int srst);
                                       
static int opendous_simple_command (urj_usbconn_libusb_param_t *params,
                                    uint8_t command,uint8_t data);


/* J-Link tap buffer functions */
static void opendous_tap_init (opendous_usbconn_data_t *data);
static int opendous_tap_execute (urj_usbconn_libusb_param_t *params);
static void opendous_tap_append_step (opendous_usbconn_data_t *data, int tms, int tdi);

/* Jlink lowlevel functions */
static int opendous_usb_message (urj_usbconn_libusb_param_t *params, int out_length, int in_length);
static int opendous_usb_write (urj_usbconn_libusb_param_t *params, unsigned int length);
static int opendous_usb_read (urj_usbconn_libusb_param_t *params);

static void opendous_debug_buffer (char *buffer, int length);

/* API functions */

void urj_tap_cable_opendous_set_frequency (urj_cable_t *cable,
                                           uint32_t frequency);

/***************************************************************************/
/* Opendous tap functions */

void
urj_tap_cable_opendous_reset (urj_usbconn_libusb_param_t *params, int trst,
                              int srst)
{
    urj_log (URJ_LOG_LEVEL_COMM, "trst=%d, srst=%d\n", trst, srst);
 
    /* Signals are active low */
    if (trst == 0) {
        opendous_simple_command (params, JTAG_CMD_SET_TRST,1);
    } else if (trst == 1) {
        opendous_simple_command (params, JTAG_CMD_SET_TRST,0);
    }

    if (srst == 0) {
        opendous_simple_command (params, JTAG_CMD_SET_SRST,1);
    } else if (srst == 1) {
        opendous_simple_command (params, JTAG_CMD_SET_SRST,0);
    }
}


static int
opendous_simple_command (urj_usbconn_libusb_param_t *params, uint8_t command, uint8_t _data)
{
    int result;
    opendous_usbconn_data_t *data = params->data;

    urj_log (URJ_LOG_LEVEL_COMM, "simple comand %#02x %#02x\n", command, _data);

    data->usb_out_buffer[0] = command;
    data->usb_out_buffer[1] = _data;
    result = opendous_usb_write (params, 2);

    if (result != 2) {
	urj_log (URJ_LOG_LEVEL_COMM, "writting: command=%#02x, data=%#02x, result=%d\n",
		 command, _data, result);
    }
    result = opendous_usb_read (params);
    if (result != 1) {
	urj_log (URJ_LOG_LEVEL_COMM, "reading: command=%#02x, result=%d\n",
		 command, result);
    }
    return data->usb_in_buffer[0];
}

/*
static int
opendous_get_status (urj_usbconn_libusb_param_t *params)
{
    //TODO: make some function for reading info
  
    return 1;
}
*/


/***************************************************************************/

static void
opendous_tap_init (opendous_usbconn_data_t *data)
{
    data->tap_length = 0;
}

static void
opendous_schedule_tap_init (opendous_usbconn_data_t *data)
{
    data->schedule_tap_length =  0;
}


static void
opendous_tap_append_step (opendous_usbconn_data_t *data, int tms, int tdi)
{
  unsigned char _tms=tms?1:0;
  unsigned char _tdi=tdi?1:0;
  
  int index = data->tap_length/4;
  int bits  = data->tap_length%4;

  if (index < OPENDOUS_TAP_BUFFER_SIZE)
  {

    if(!bits)
      data->tms_buffer[index]=0;
    
    data->tms_buffer[index]  |= (_tdi<<(bits*2))|(_tms<<(bits*2+1)) ;
    data->tap_length++;
  }
  else
  {
    ERROR ("opendous_tap_append_step, overflow\n");
  }
}

static void
opendous_schedule_tap_append_step (opendous_usbconn_data_t *data, int tms, int tdi)
{
  unsigned char _tms=tms?1:0;
  unsigned char _tdi=tdi?1:0;
  
  int index = data->schedule_tap_length/4;
  int bits  = data->schedule_tap_length%4;

  if (index < OPENDOUS_SCHEDULE_BUFFER_SIZE)
  {

    if(!bits)
      data->schedule_tap[index]=0;
    
    data->schedule_tap[index]  |= (_tdi<<(bits*2))|(_tms<<(bits*2+1)) ;
    data->schedule_tap_length++;
  }
  else
  {
    ERROR ("schedule_opendous_tap_append_step, overflow\n");
  }
}


/* Send a tap sequence to the device, and receive the answer */

static int
opendous_tap_execute (urj_usbconn_libusb_param_t *params)
{
    opendous_usbconn_data_t *data = params->data;
    int byte_length,byte_length_out;
    int i;
    int result;
    /*int bit_length;*/
    //if(debug_log) fprintf (debug_log,"TAP execute:%d\n",data->tap_length);
    if (data->tap_length > 0)
    {

        byte_length =     (data->tap_length+3)/4;
        byte_length_out = (data->tap_length+7)/8;
        data->usb_out_buffer[0]=JTAG_CMD_TAP_OUTPUT | ((data->tap_length%4)<<4); //transfer command

        for (i = 0; i < byte_length; i++)
        {
            data->usb_out_buffer[i+1] = data->tms_buffer[i];
        }
        
        result = opendous_usb_message (params, byte_length+1, byte_length_out);

        if (result == byte_length_out)
        {
          data->last_tdo = (data->usb_in_buffer[byte_length_out - 1])&(1<< (data->tap_length%8) )? 1 : 0;
          //opendous_debug_buffer(data->usb_in_buffer,byte_length);
        } else {
            ERROR ("opendous_tap_execute, wrong result %d, expected %d\n",
                   result, byte_length_out);

            return -2;
        }
        opendous_tap_init (data);
    }
    return 0;
}

static int
opendous_schedule_flush (urj_usbconn_libusb_param_t *params)
{
    opendous_usbconn_data_t *data = params->data;
    int byte_length,byte_length_out;
    /*int i;*/
    int result;
    int bit_length=data->schedule_tap_length;
#ifdef DEBUG_TRANSFER_STATS  
    if(debug_log) fprintf (debug_log,"f:%d\t",data->schedule_tap_length);
#endif //DEBUG_TRANSFER_STATS  
    int out_offset=0;
    int in_offset=0;
    while(bit_length>0)
    {

        byte_length = (bit_length+3)/4;
      
        if(byte_length<(OPENDOUS_IN_BUFFER_SIZE-2))     
        {
          byte_length_out = (bit_length+7)/8;
          data->usb_out_buffer[0]=JTAG_CMD_TAP_OUTPUT | ((bit_length%4)<<4); //transfer command
          bit_length=0;
        } else {
          byte_length=(OPENDOUS_IN_BUFFER_SIZE-2);
          data->usb_out_buffer[0]=JTAG_CMD_TAP_OUTPUT ; //transfer command
          
          bit_length-=(OPENDOUS_IN_BUFFER_SIZE-2)*4;
          byte_length_out = (OPENDOUS_IN_BUFFER_SIZE-2)/2;
        }

        //for (i = 0; i < byte_length; i++)
        //{
        //   data->usb_out_buffer[i+1] = data->tms_buffer[i+out_offset];
        //}
        memmove(data->usb_out_buffer+1,data->schedule_tap+in_offset,byte_length);
        in_offset+=byte_length;
        
        result = opendous_usb_message (params, byte_length+1, byte_length_out);

        if (result == byte_length_out)
        {
          //data->last_tdo = (data->usb_in_buffer[byte_length_out - 1])&(1<< (data->tap_length%8) )? 1 : 0;
          //opendous_debug_buffer(data->usb_in_buffer,byte_length);
          memmove(data->schedule_tdo+out_offset,data->usb_in_buffer,byte_length_out);
          out_offset+=byte_length_out;
        } else {
            ERROR ("opendous_schedule_flush, wrong result %d, expected %d\n",
                   result, byte_length_out);

            return -2;
        }
    }
    opendous_schedule_tap_init (data);
    return 0;
}


/* ---------------------------------------------------------------------- */

/* Send a message and receive the reply. */
static int
opendous_usb_message (urj_usbconn_libusb_param_t *params, int out_length, int in_length)
{
    int result;

    result = opendous_usb_write (params, out_length);
    if (result == out_length)
    {
        result = opendous_usb_read (params);
        if (result == in_length)
        {
            return result;
        } else {
            ERROR ("OPENDOUS usb_bulk_read failed (requested=%d, result=%d)\n",
                   in_length, result);

            return -1;
        }
    } else {
        ERROR ("OPENDOUS  usb_bulk_write failed (requested=%d, result=%d)\n",
               out_length, result);

        return -1;
    }
}

/* ---------------------------------------------------------------------- */

/* Write data from out_buffer to USB. */
static int
opendous_usb_write (urj_usbconn_libusb_param_t *params, unsigned int out_length)
{
    int result, transferred;
    opendous_usbconn_data_t *data;
    data = params->data;

    urj_log (URJ_LOG_LEVEL_ALL, "out_length=%d\n", out_length);
    if (out_length > OPENDOUS_OUT_BUFFER_SIZE) {
        urj_log (URJ_LOG_LEVEL_ERROR,
		 "opendous_jtag_write illegal out_length=%d (max=%d)\n",
		 out_length, OPENDOUS_OUT_BUFFER_SIZE);
        return -1;
    }

    urj_log (URJ_LOG_LEVEL_ALL, "buffer contains:\n");
    if (URJ_LOG_LEVEL_ALL >= urj_log_state.level) {
	opendous_debug_buffer (data->usb_out_buffer, out_length);
    }
   
    urj_log(URJ_LOG_LEVEL_ALL, "dev: %#p, ep=%#02x, buff: %#p, size=%d, timeout=%d\n",
	    params->handle, OPENDOUS_WRITE_ENDPOINT, data->usb_out_buffer,
	    out_length, OPENDOUS_USB_TIMEOUT);
    result = libusb_bulk_transfer (params->handle,
                                   OPENDOUS_WRITE_ENDPOINT,
                                   data->usb_out_buffer,
                                   out_length,
                                   &transferred,
                                   OPENDOUS_USB_TIMEOUT);
#ifdef HAVE_LIBUSB1
    if (result) {
        urj_log (URJ_LOG_LEVEL_DEBUG, "libusb_error_name='%s'\n", libusb_error_name(result));
    }
#endif
    urj_log (URJ_LOG_LEVEL_DETAIL, "result=%d, length=%d, transferred=%d\n",
	    result, out_length, transferred);

    return transferred;
}

/* ---------------------------------------------------------------------- */

/* Read data from USB into in_buffer. */
static int
opendous_usb_read (urj_usbconn_libusb_param_t *params)
{
    opendous_usbconn_data_t *data = params->data;
    int transferred;
    urj_log(URJ_LOG_LEVEL_ALL, "dev: %#p, ep=%#02x, buff: %#p, size=%d, timeout=%d\n",
	    params->handle, OPENDOUS_READ_ENDPOINT, data->usb_in_buffer,
	    OPENDOUS_IN_BUFFER_SIZE, OPENDOUS_USB_TIMEOUT);
    int result = libusb_bulk_transfer (params->handle,
                                       OPENDOUS_READ_ENDPOINT,
                                       data->usb_in_buffer,
                                       OPENDOUS_IN_BUFFER_SIZE,
                                       &transferred,
                                       OPENDOUS_USB_TIMEOUT);
#ifdef HAVE_LIBUSB1
    if (result) {
      urj_log (URJ_LOG_LEVEL_DEBUG, "libusb_error_name='%s'\n", libusb_error_name(result));
    }
#endif
    urj_log (URJ_LOG_LEVEL_ALL, "result=%d, transferred=%d\n", result, transferred);
    urj_log (URJ_LOG_LEVEL_ALL, "Have read:\n");
    if (URJ_LOG_LEVEL_ALL >= urj_log_state.level) {
	opendous_debug_buffer (data->usb_in_buffer, transferred);
    }
    return transferred;
}

/* ---------------------------------------------------------------------- */

#define BYTES_PER_LINE  16

static void
opendous_debug_buffer (char *buffer, int length)
{
    char line[81];
    char s[4];
    int i;
    int j;

    for (i = 0; i < length; i += BYTES_PER_LINE)
    {
        snprintf (line, 5, "%04x", i);
        for (j = i; j < i + BYTES_PER_LINE && j < length; j++)
        {
            snprintf (s, 4, " %02x", buffer[j]);
            strcat (line, s);
        }
        INFO (line);
        INFO ("\n");
    }
}


/* ---------------------------------------------------------------------- */

static int
opendous_init (urj_cable_t *cable)
{
    int result;
    urj_usbconn_libusb_param_t *params;
    opendous_usbconn_data_t *data;

    params = cable->link.usb->params;
    params->data = malloc (sizeof (opendous_usbconn_data_t));
    if (params->data == NULL)
    {
	urj_error_set(URJ_ERROR_OUT_OF_MEMORY, "malloc(%zd) fails",
		      sizeof(opendous_usbconn_data_t));
	return URJ_STATUS_FAIL;
    }
    data = params->data;
    memset (data, 0, sizeof(*data));	/*be paranoid*/

    if (urj_tap_usbconn_open (cable->link.usb) != URJ_STATUS_OK) {
	urj_log (URJ_LOG_LEVEL_ERROR, "Failed to open\n");
	free(data);
	return URJ_STATUS_FAIL;
    }

    data->last_tdo = 0;
    opendous_tap_init (data);

    //result = opendous_usb_read(params); /*WAS NOT THERE*/
    //urj_log (URJ_LOG_LEVEL_DEBUG, "result=%d\n", result);

    urj_log (URJ_LOG_LEVEL_DETAIL, "OPENDOUS JTAG Interface ready\n");
    urj_tap_cable_opendous_set_frequency (cable, 4E6);
    urj_tap_cable_opendous_reset (params, 0, 0);
    
#ifdef DEBUG_TRANSFER_STATS    
    debug_log=fopen("/tmp/Debug-log.txt","at");
#endif     

    return URJ_STATUS_OK;
}

/* ---------------------------------------------------------------------- */

static void
opendous_free (urj_cable_t *cable)
{
    opendous_usbconn_data_t *data;
    data = ((urj_usbconn_libusb_param_t *) (cable->link.usb->params))->data;
    free (data);
#ifdef DEBUG_TRANSFER_STATS  
    if(debug_log) fclose(debug_log);
    debug_log=NULL;
#endif //DEBUG_TRANSFER_STATS  
    urj_tap_cable_generic_usbconn_free (cable);
}

/* ---------------------------------------------------------------------- */

void
urj_tap_cable_opendous_set_frequency (urj_cable_t *cable, uint32_t frequency)
{
    /*int result;*/
    int speed = frequency / 1E3;
    /*urj_usbconn_libusb_param_t *params = cable->link.usb->params;*/
    /*opendous_usbconn_data_t *data = params->data;*/

    if (1 <= speed && speed <= OPENDOUS_MAX_SPEED)
    {
      //TODO: convert speed into delay
      
    }
    else
    {
        INFO ("Requested speed %dkHz exceeds maximum of %dkHz, ignored\n",
              speed, OPENDOUS_MAX_SPEED);
    }
}

/* ---------------------------------------------------------------------- */

static void
opendous_clock (urj_cable_t *cable, int tms, int tdi, int n)
{
    int i;
    urj_usbconn_libusb_param_t *params = cable->link.usb->params;
    opendous_usbconn_data_t *data = params->data;
    for (i = 0; i < n; i++)
    {
      opendous_tap_append_step (data, tms, tdi);
      if (data->tap_length >= (OPENDOUS_TAP_BUFFER_SIZE*4))
          opendous_tap_execute (params);
    }
    opendous_tap_execute (params);
}

static void
opendous_schedule_clock (urj_cable_t *cable, int tms, int tdi, int n)
{
    int i;
    urj_usbconn_libusb_param_t *params = cable->link.usb->params;
    opendous_usbconn_data_t *data = params->data;
    for (i = 0; i < n; i++)
    {
      opendous_schedule_tap_append_step (data, tms, tdi);
    }
}


/* ---------------------------------------------------------------------- */

static int
opendous_get_tdo (urj_cable_t *cable)
{
  urj_usbconn_libusb_param_t *params = cable->link.usb->params;
  opendous_usbconn_data_t *data = params->data;
  // TODO: This is the TDO _before_ last clock occured
  // ...   Anyone knows how to get the current TDO state?
  return data->last_tdo;
}

/* ---------------------------------------------------------------------- */
static void
opendous_copy_out_data (opendous_usbconn_data_t *data, int len, int offset,
                        char *buf)
{
    int i;
    for (i = 0; i < len; i++)
    {
        int bit = (1<<(i&7));
        int byte = i>>3;
        buf[offset + i] = (data->usb_in_buffer[byte] & bit) ? 1 : 0;
    }
}

static int
opendous_transfer (urj_cable_t *cable, int len, const char *in, char *out)
{
    int i, j;
    urj_usbconn_libusb_param_t *params = cable->link.usb->params;
    opendous_usbconn_data_t *data = params->data;

    //INFO ("Opendous transfer len:%d\n",len);
    for (j = 0, i = 0; i < len; i++)
    {
        opendous_tap_append_step (data, 0, in[i]);

        if (data->tap_length >= OPENDOUS_TAP_BUFFER_SIZE*4)
        {
            opendous_tap_execute (params);
            if (out)
                opendous_copy_out_data (data, i - j, j, out);
            j = i;
        }
    }
    if (data->tap_length > 0)
    {
        opendous_tap_execute (params);
        if (out)
            opendous_copy_out_data (data, i - j, j, out);
    }
    return len;
}

static int
opendous_schedule_transfer (urj_cable_t *cable, int len, char *in)
{
    int i;
    urj_usbconn_libusb_param_t *params = cable->link.usb->params;
    opendous_usbconn_data_t *data = params->data;

    for (i = 0; i < len; i++)
    {
        opendous_schedule_tap_append_step (data, 0, in[i]);

    }
    return len;
}



/* ---------------------------------------------------------------------- */

static int
opendous_set_signal (urj_cable_t *cable, int mask, int val)
{
  
  return 1; 
}

static void
opendous_flush (urj_cable_t *cable, urj_cable_flush_amount_t how_much )
{
  urj_usbconn_libusb_param_t *params = cable->link.usb->params;
  opendous_usbconn_data_t *data=(opendous_usbconn_data_t*)params->data;
  
	if (how_much == URJ_TAP_CABLE_OPTIONALLY) return;
  if (how_much == URJ_TAP_CABLE_TO_OUTPUT && cable->done.num_items>0) return;
  
  opendous_schedule_tap_init(data);
#ifdef DEBUG_TRANSFER_STATS  
  if(debug_log) fprintf (debug_log,"ff:%d %d\t",how_much,cable->todo.num_items);  
#endif //DEBUG_TRANSFER_STATS
  
	while (cable->todo.num_items > 0) //???
	{
		int i, j, n;

		for (j = i = cable->todo.next_item, n = 0; n < cable->todo.num_items; n++)
		{

			switch (cable->todo.data[i].action)
			{
			case URJ_TAP_CABLE_CLOCK:
				opendous_schedule_clock( cable,
				                           cable->todo.data[i].arg.clock.tms,
				                           cable->todo.data[i].arg.clock.tdi,
				                           cable->todo.data[i].arg.clock.n );
				break;

			case URJ_TAP_CABLE_GET_TDO:
       				 break;

			case URJ_TAP_CABLE_TRANSFER:
				opendous_schedule_transfer( cable,
                                    cable->todo.data[i].arg.transfer.len,
                                    cable->todo.data[i].arg.transfer.in);
				break;

			default:
				break;
			}

			i++;
			if (i >= cable->todo.max_items)
				i = 0;
		}
    

		opendous_schedule_flush(params);
    int bit_pos=0;
		while (j != i)
		{
			switch (cable->todo.data[j].action)
			{
			case URJ_TAP_CABLE_GET_TDO:
				{
					int m;
					m = urj_tap_cable_add_queue_item( cable, &(cable->done) );
					cable->done.data[m].action = URJ_TAP_CABLE_GET_TDO;
					cable->done.data[m].arg.value.val = data->schedule_tdo[bit_pos/8]&(1<<(bit_pos%8))?1:0;
					break;
				}
			case URJ_TAP_CABLE_GET_SIGNAL:
				{
					int m = urj_tap_cable_add_queue_item( cable, &(cable->done) );
					cable->done.data[m].action = URJ_TAP_CABLE_GET_SIGNAL;
					cable->done.data[m].arg.value.sig = cable->todo.data[j].arg.value.sig;
					if (cable->todo.data[j].arg.value.sig == URJ_POD_CS_TRST)
						cable->done.data[m].arg.value.val = 1;
					else
						cable->done.data[m].arg.value.val = -1; // not supported yet
					break;
				}
      case URJ_TAP_CABLE_CLOCK:
        bit_pos+=cable->todo.data[j].arg.clock.n;
        break;
			case URJ_TAP_CABLE_TRANSFER:
				{
					free( cable->todo.data[j].arg.transfer.in );
					if (cable->todo.data[j].arg.transfer.out)
					{
            int k;
						int m = urj_tap_cable_add_queue_item( cable, &(cable->done) );
						if (m < 0)
							fprintf(stderr,"out of memory!\n");
            
						cable->done.data[m].action = URJ_TAP_CABLE_TRANSFER;
						cable->done.data[m].arg.xferred.len = cable->todo.data[j].arg.transfer.len;
						cable->done.data[m].arg.xferred.res = 0;
            
						cable->done.data[m].arg.xferred.out = cable->todo.data[j].arg.transfer.out;
            for(k=0;k< cable->todo.data[j].arg.transfer.len;k++)
            {
              int offset=bit_pos+k;
              cable->todo.data[j].arg.transfer.out[k]=
								data->schedule_tdo[offset/8]&(1<<(offset%8))?1:0;
            }
					}
					bit_pos+=cable->todo.data[j].arg.transfer.len;
				}
			default:
				break;
			}

			j++;
			if (j >= cable->todo.max_items)
				j = 0;
			cable->todo.num_items--;
		}
    
    data->last_tdo = (data->schedule_tdo[bit_pos/8])&(1<<(bit_pos%8))? 1 : 0;
		cable->todo.next_item = i;
	}
  
}


const urj_cable_driver_t urj_tap_cable_opendous_driver = {
	"opendous",
	N_("Opendous based JTAG"),
	URJ_CABLE_DEVICE_USB,
	{ .usb = urj_tap_cable_generic_usbconn_connect, },
	urj_tap_cable_generic_disconnect,
	opendous_free,
	opendous_init,
	urj_tap_cable_generic_usbconn_done,
	urj_tap_cable_opendous_set_frequency,
	opendous_clock,
	opendous_get_tdo,
	opendous_transfer,
	opendous_set_signal,
	urj_tap_cable_generic_get_signal,
	opendous_flush,
	urj_tap_cable_generic_usbconn_help
};

// (vid, pid, driver, name, cable)
URJ_DECLARE_USBCONN_CABLE(0x03eb, 0x204f, "libusb", "opendous", opendous)

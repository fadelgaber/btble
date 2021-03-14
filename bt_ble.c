/*
 *  paulvha / November 2020 / version 1.0
 *
 *
 * *******************************************************************
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2010  Nokia Corporation
 *  Copyright (C) 2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 ******************************************************************************

 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include <glib.h>

#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "lib/hci_lib.h"
#include "lib/sdp.h"
#include "lib/uuid.h"

#include "att.h"                                // error codes
#include "btio/btio.h"
#include "gattrib.h"
#include "gatt.h"
#include "bt_ble.h"

// DEFINE THE UUID (defined in Ambiq SDK - att_uuid.h)
static char accel_prim[]="e0262760-08c2-11e1-9073-0e8ac72e1001";                 // UUID primary service for Dats
static char accel_char[]="e0262760-08c2-11e1-9073-0e8ac72e0001";                 // UUID characteristic dats
uint16_t accel_handle;

static char *opt_src = NULL;                    // store local MAC
static char *opt_dst = NULL;                    // store destination MAC
static char *opt_dst_type = NULL;               // type : public ?
static char *opt_sec_level = NULL;              // security level
static int opt_mtu = 0;                         // set MTU size
static int opt_psm = 0;                         // set GATT over BD/EDR
static GMainLoop *event_loop;                   // mainloop
static gboolean got_error = FALSE;              // indicate error for exit condition
static GSourceFunc operation;                   // place to store operation function called from connect_cb
static GAttrib *b_attrib;                       // attrib

static gboolean g_debug = FALSE;                // set verbose / debug

#define MAX_PRIMARY 20
struct gatt_primary r_primary[MAX_PRIMARY];     // stored received primary

// some forward declarations
static gboolean read_all_primary();
static void request_data();
void handle_received_data(const uint8_t *data, uint16_t len);

/**
 *  @brief call back from enabling notifications.
 */
static void enable_comms_cb(guint8 status, const guint8 *pdu, guint16 plen,
                            gpointer user_data)
{
    if (g_debug) g_print("%s\n", __func__);

    if (status != 0) {
        g_printerr("Notification Write Request failed: %s\n", att_ecode2str(status));
        goto error;
    }

    // now sent request to server
    request_data();

    return;

error:
    got_error = TRUE;
    g_main_loop_quit(event_loop);
}

/**
 * @brief write the CCC to enable transmitting data notificatons
 */
static void enable_comms()
{
    if (g_debug) g_print("%s\n", __func__);

    uint16_t handle;
    uint8_t value[2];

    value[0]=0x01;      // enable notification
    value[1]=0x00;

    // enable TX notify (CCCD)
    handle = accel_handle+1;

    // will return in enable_comms_cb()
    gatt_write_char(b_attrib, handle, value, 2, enable_comms_cb, NULL);
}

/**
 *  @brief notifications handler
 *  called when data or acknowledgement was received from server as notification
 *  is called from gattrib.c / attrib_callback_notify()
 */
static void events_handler(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
    GAttrib *attrib = user_data;
    uint8_t *opdu;
    uint16_t handle, i, olen = 0;
    size_t plen;

    if (g_debug) g_print("%s\n", __func__);

    handle = pdu[2] <<8 | pdu[1];

    switch (pdu[0]) {

        case ATT_OP_HANDLE_NOTIFY:

            if (g_debug) {
                g_print("Notification on handle 0x%04X\n", handle);
            }

            // NOW do SOMETHING with your data
            if (handle == accel_handle) {
                handle_received_data(&pdu[3], len-3);  // skip header
                goto done;                              // return to main() and wait before sending next request
            }                                           // if server is in streaming mode set return
            break;

        case ATT_OP_HANDLE_IND: // info only.. not used at this time . Future ???
            g_print("Indication on handle 0x%04X \n", handle);
            break;

        default:
            g_print("Invalid event on handle 0x%04X \n", handle);
            break;
    }

    for (i = 3; i < len; i++)  g_print("%02x ", pdu[i]);
    g_print(" Ignored\n");

    // sent something back on indication request
    opdu = g_attrib_get_buffer(attrib, &plen);
    olen = enc_confirmation(opdu, plen);

    if (olen > 0)
        g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);

done:
   g_main_loop_quit(event_loop);
}

/**
 * @brief start notification & indication listeners
 */
static gboolean listen_start(gpointer user_data)
{
    if (g_debug) g_print("%s\n", __func__);

    g_attrib_register(b_attrib, ATT_OP_HANDLE_NOTIFY, GATTRIB_ALL_HANDLES,
                        events_handler, b_attrib, NULL);
    g_attrib_register(b_attrib, ATT_OP_HANDLE_IND, GATTRIB_ALL_HANDLES,
                        events_handler, b_attrib, NULL);

    // if the function returns FALSE it is automatically removed from the list of event sources and will not be called again.
    return FALSE;
}

/* call back after connect has been completed */
static void connect_cb(GIOChannel *io, GError *err, gpointer user_data)
{
    uint16_t mtu;
    uint16_t cid;
    GError *gerr = NULL;

    if (g_debug) g_print("%s\n", __func__);

    if (err) {
        g_printerr("%s\n", err->message);
        got_error = TRUE;
        g_main_loop_quit(event_loop);
    }

    // negotiate the MTU size
    bt_io_get(io, &gerr, BT_IO_OPT_IMTU, &mtu,
                BT_IO_OPT_CID, &cid, BT_IO_OPT_INVALID);

    if (gerr) {
        g_printerr("Can't detect MTU, using default: %s",
                                gerr->message);
        g_error_free(gerr);
        mtu = ATT_DEFAULT_LE_MTU;
    }

    if (cid == ATT_CID)
        mtu = ATT_DEFAULT_LE_MTU;

    b_attrib = g_attrib_new(io, mtu, false);

    // start notification listeners to call events_handler when
    // any data is received
    g_idle_add(listen_start,b_attrib);

    // call the first function after connect
    operation(b_attrib);
}

// call back after sending data
static void char_write_req_cb(guint8 status, const guint8 *pdu, guint16 plen,
                            gpointer user_data)
{
    if (g_debug) g_print("%s\n", __func__);

    if (status) {
        g_printerr("Sending requestfailed: %s\n", att_ecode2str(status));
        got_error = TRUE;
        goto done;
    }

    return;
done:
   g_main_loop_quit(event_loop);
}

/* trigger a request...
 * Any data will return as notification, thus triggering events_handler
 */
static void request_data()
{
   uint8_t value;       // not used but needed for the call

   if (g_debug) g_print("%s\n", __func__);

   // call back 'char_write_req_cb' when done
   gatt_write_char(b_attrib, accel_handle, &value, 1, char_write_req_cb, NULL);
}

/* call back as services handles are extended with characteristic information
 * now lookup the wanted characteristic, obtain the specific handle to read the value
 */
static void read_characteristics(uint8_t status, GSList *ranges, void *user_data)
{
    GSList *l;
    uint8_t value;

   if (g_debug) g_print("%s\n", __func__);

    if (status) {
        g_printerr("Discover characteristics failed: %s\n", att_ecode2str(status));
        got_error = TRUE;
        goto error;
    }

    // try to find a match for the characteristic
    for (l = ranges; l; l = l->next) {
        struct gatt_desc *range = l->data;

        // If UUID for value
        if (strcmp (range->uuid, accel_char) == 0) {

            if (g_debug)  g_print("FOUND: handle = 0x%04x, uuid = %s\n", range->handle, range->uuid);
            accel_handle = range->handle;

            /** OK, so we are connected, we have found the service and validated the handles */
            // enable Notification communication on the server
            enable_comms();

            return;
        }
    }

    // oh  oh.. not good
    g_printerr("Did not find the %s value UUID\n", accel_char);
    got_error = TRUE;

error:
    g_main_loop_quit(event_loop);
}

/* lookup the wanted service UUID and it's handles
 * then read the handles to obtain each characteristic info / UUID
 */
static gboolean read_handles()
{
    int i;

    if (g_debug) g_print("%s\nSearch for uuid %s\n", __func__,  accel_prim);

    // try to find service in local list
    for (i=0; i < MAX_PRIMARY; i++) {

        // lookup UUID
        if (strcmp (r_primary[i].uuid, accel_prim) == 0)
        {
            // Get handles for service : call back read_characteristics when done
            gatt_discover_desc(b_attrib,r_primary[i].range.start, r_primary[i].range.end, NULL, read_characteristics, NULL);

            if (g_debug)  g_print("FOUND start handle = 0x%04x, end handle = 0x%04x "
            "uuid: %s\n", r_primary[i].range.start, r_primary[i].range.end, r_primary[i].uuid);

            return(TRUE);
        }

        if (r_primary[i].range.end == 0x0) break;
     }

    // oh oh not good !!
    g_printerr("Service not found on this device\n");

    got_error = TRUE;

done:
    g_main_loop_quit(event_loop);
}

/* First obtain all primary services and handles
 *
 * Multiple values / characteristiscs can be requested
 * but once connnected the device will stop advertising services
 * so we store all available primary services info in the first connect
 */
static void extract_all_primary(uint8_t status, GSList *services, void *user_data)
{
    GSList *l;
    uint8_t i;

    if (g_debug) g_print("%s\n", __func__);

    if (status) {
        g_printerr("Discover all primary services failed: %s\n",  att_ecode2str(status));
        got_error = TRUE;
        goto done;
    }

    // copy primary services list to local structure
    for (l = services, i=0; l && i < MAX_PRIMARY; l = l->next, i++) {
        struct gatt_primary *prim = l->data;
        r_primary[i].range.start = prim->range.start;
        r_primary[i].range.end = prim->range.end;
        strcpy(r_primary[i].uuid,prim->uuid);

         if (g_debug){
            g_print("attr handle = 0x%04x, end grp handle = 0x%04x "
            "uuid: %s\n", prim->range.start, prim->range.end, prim->uuid);
        }
    }

    if (i == MAX_PRIMARY) {
        g_printerr("Exceeded maximum primary amount of %d:\n",MAX_PRIMARY);
        got_error = TRUE;
        goto done;
    }
    else     // terminate list
        r_primary[i].range.end = 0x0;

    // obtain all the handles for a specific service
    read_handles();

    return;
done:
    g_main_loop_quit(event_loop);
}

/* First obtain the all primary services and handles
 *
 * Multiple values / characteristiscs can be requested
 * but once connnected the device will stop advertising services
 * so we request them all up front after the first connect
 */
static gboolean read_all_primary()
{
    if (g_debug) g_print("%s\n", __func__);

    // set call back for extract_all_primary
    gatt_discover_primary(b_attrib, NULL, extract_all_primary, NULL);

    return(TRUE);
}

static GOptionEntry options[] = {
    { "adapter", 'i', 0, G_OPTION_ARG_STRING, &opt_src,
        "Specify local adapter interface", "hciX" },
    { "device", 'b', 0, G_OPTION_ARG_STRING, &opt_dst,
        "Specify remote Bluetooth address", "MAC" },
    { "addr-type", 't', 0, G_OPTION_ARG_STRING, &opt_dst_type,
        "Set LE address type. Default: public", "[public | random]"},
    { "mtu", 'm', 0, G_OPTION_ARG_INT, &opt_mtu,
        "Specify the MTU size", "MTU" },
    { "psm", 'p', 0, G_OPTION_ARG_INT, &opt_psm,
        "Specify the PSM for GATT/ATT over BR/EDR", "PSM" },
    { "sec-level", 'l', 0, G_OPTION_ARG_STRING, &opt_sec_level,
        "Set security level. Default: low", "[low | medium | high]"},
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &g_debug,
        "Set verbose Default: off", NULL},
    { NULL },
};

int main(int argc, char *argv[])
{
    GOptionContext *context;
    GError *gerr = NULL;
    GIOChannel *chan;

    // default values
    opt_dst_type = g_strdup("public");
    opt_sec_level = g_strdup("low");

    // add options and parse command line
    context = g_option_context_new(NULL);
    g_option_context_add_main_entries(context, options, NULL);

    if (!g_option_context_parse(context, &argc, &argv, &gerr)) {
        g_printerr("%s\n", gerr->message);
        g_clear_error(&gerr);
    }

    if (g_debug) g_print("Trying to Connect\n");

    if (opt_dst == NULL) {
        g_printerr("Remote Bluetooth address required\n");
        got_error = TRUE;
        goto done;
    }

    chan = gatt_connect(opt_src, opt_dst, opt_dst_type, opt_sec_level,
                    opt_psm, opt_mtu, connect_cb, &gerr);

    if (chan == NULL) {
        g_printerr("%s\n", gerr->message);
        g_clear_error(&gerr);
        got_error = TRUE;
        goto done;
    }

    //g_print("Starting mainloop (waiting on connect)\n");

    // set operation to perform : called at end of connect_cb
    operation = read_all_primary;

    // NULL = the default context will be used   FALSE = Not running
    event_loop = g_main_loop_new(NULL, FALSE);

    // main loop here
    // first connect and get all primary services
    // next requested values for data
    bool FirstConnect = TRUE;
    while (1) {

        if (FirstConnect) FirstConnect = FALSE;
        else request_data();

        g_main_loop_run(event_loop);

        if (got_error) goto done;       // stop if error encountered

        //sleep(1);                       // delay between polling
    }

done:
    if (event_loop != NULL) g_main_loop_unref(event_loop);
    g_option_context_free(context);
    g_free(opt_src);
    g_free(opt_dst);
    g_free(opt_sec_level);

    if (got_error)
        exit(EXIT_FAILURE);
    else
        exit(EXIT_SUCCESS);
}

/**
 * @brief : translate 4 bytes to float IEEE754
 * @param data : data buffer to read from
 * @param offset : offset in data
 *
 * return : float number
 */
float byte_to_float(const uint8_t *data, int offset)
{
    uint8_t i;
    union {
        uint8_t array[4];
        float value;
    } conv;

    for (i = 0; i < 4; i++){
       conv.array[3-i] = data[offset+i]; //or conv.array[i] = data[offset+i]; depending on endianness
    }

    return conv.value;
}

/**
 * handle the received to display
 * @param data : data portion of the pdu
 * @param len : length of the data portion
 */
void handle_received_data(const uint8_t *data, uint16_t len)
{
    uint16_t i;

    if(g_debug){

        g_print("Received data : ");
        for (i = 3; i < len; i++)  g_print("%02x ", data[i]);
        g_print("\n");
    }
    /**
     * this is THE place to perform your output,
     * looking at the example in dats_main.c/ datsSendData()
     * on the server
     */

   if (len != 12){
      g_print("Not enough data, got %d, expected 12\n", len);
      return;
   }

   // this could be defined global of course
   float acceleration_mg[3];

   // now turn the float to bytes
   acceleration_mg[0] = byte_to_float(data, 0);
   acceleration_mg[1] = byte_to_float(data, 4);
   acceleration_mg[2] = byte_to_float(data, 8);

   if(acceleration_mg[0]==0.937500) return;

   if(acceleration_mg[0]==00.00){

    if(acceleration_mg[1]>5 || acceleration_mg[1]<0) return;
    if(acceleration_mg[2]>1 || acceleration_mg[2]<0) return;

    g_print("Gesture detected index %04.2f score %04.2f\r\n", acceleration_mg[1], acceleration_mg[2]);
   }
   else{
    g_print("%04.2f,%04.2f,%04.2f\r\n", acceleration_mg[0], acceleration_mg[1], acceleration_mg[2]);
   }

   
}

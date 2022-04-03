/* Daikin app */
/* Copyright �2022 Adrian Kennard, Andrews & Arnold Ltd. See LICENCE file for details .GPL 3.0 */

static __attribute__((unused))
const char TAG[] = "Daikin";

#include "revk.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include <driver/gpio.h>
#include <driver/uart.h>

// The following define the controls and status for the Daikin, using macros
// e(name,tags) Enumerated value, uses single character from tags, e.g. FHCA456D means "F" is 0, "H" is 1, etc
// b(name)      Boolean
// t(name)      Temperature
// s(name,len)  String (for status only, e.g. model)

// Daikin controls
#define	accontrol		\
	b(power)		\
	e(mode,FHCA456D)	\
	t(temp)			\
	e(fan,A12345)		\


// Daikin status
#define acstatus		\
	b(online)		\
	e(compressor,XHC)	\
	s(model,20)		\

// Macros for setting values
#define	daikin_set_b(name,value)	daikin_set_value(#name,&daikin.name,CONTROL_##name,value)
#define	daikin_set_e(name,value)	daikin_set_enum(#name,&daikin.name,CONTROL_##name,value,CONTROL_##name##_VALUES)
#define	daikin_set_t(name,value)	daikin_set_temp(#name,&daikin.name,CONTROL_##name,value)

// Settings (RevK library used by MQTT setting command)
#define	settings		\
	b(debug)	\
	b(dump)	\
	u8(uart,1)	\
	io(tx,CONFIG_DAIKIN_TX)	\
	io(rx,CONFIG_DAIKIN_RX)	\

#define u32(n,d)        uint32_t n;
#define s8(n,d) int8_t n;
#define u8(n,d) uint8_t n;
#define b(n) uint8_t n;
#define s(n) char * n;
#define io(n,d)           uint8_t n;
settings
#undef io
#undef u32
#undef s8
#undef u8
#undef b
#undef s
#define PORT_INV 0x40
#define port_mask(p) ((p)&63)
enum {                          // Number the control fields
#define	b(name)		CONTROL_##name##_pos,
#define	t(name)		b(name)
#define	e(name,values)	b(name)
#define	s(name,len)	b(name)
   accontrol acstatus
#undef	b
#undef	t
#undef	e
#undef	s
};
#define	b(name)		const uint64_t CONTROL_##name=(1ULL<<CONTROL_##name##_pos);
#define	t(name)		b(name)
#define	e(name,values)	b(name) const char CONTROL_##name##_VALUES[]=#values;
#define	s(name,len)	b(name)
accontrol acstatus
#undef	b
#undef	t
#undef	e
#undef	s
// The current state
struct {
   SemaphoreHandle_t mutex;     // Control changes
   uint64_t control_changed;    // Which control fields are being set
   uint8_t control_count;       // How many times we have tried to change control and not worked yet
#define	b(name)		uint8_t	name;
#define	t(name)		float name;
#define	e(name,values)	uint8_t name;
#define	s(name,len)	char name[len];
   accontrol acstatus
#undef	b
#undef	t
#undef	e
#undef	s
    uint8_t talking:1;          // We are getting answers
   uint8_t status_changed:1;    // Status has changed

} daikin;

const char *daikin_set_value(const char *name, uint8_t * ptr, uint64_t flag, uint8_t value)
{                               // Setting a value (uint8_t)
   if (*ptr == value)
      return NULL;              // No change
   xSemaphoreTake(daikin.mutex, portMAX_DELAY);
   *ptr = value;
   daikin.control_changed |= flag;
   xSemaphoreGive(daikin.mutex);
   return NULL;
}

const char *daikin_set_enum(const char *name, uint8_t * ptr, uint64_t flag, char *value, const char *values)
{                               // Setting a value (uint8_t) based on string which is expected to be single character from values set
   if (!value || !*value)
      return "No value";
   if (value[1])
      return "Value is meant to be one character";
   char *found = strchr(values, *value);
   if (!found)
      return "Value is not a valid value";
   daikin_set_value(name, ptr, flag, (int) (found - values));
   return NULL;
}

const char *daikin_set_temp(const char *name, float *ptr, uint64_t flag, float value)
{                               // Setting a value (float)
   if (*ptr == value)
      return NULL;              // No change
   xSemaphoreTake(daikin.mutex, portMAX_DELAY);
   *ptr = value;
   daikin.control_changed |= flag;
   xSemaphoreGive(daikin.mutex);
   return NULL;
}

void daikin_response(uint8_t cmd, int len, uint8_t * payload)
{                               // Process response
   if (debug)
   {
      jo_t j = jo_object_alloc();
      jo_stringf(j, "cmd", "%02X", cmd);
      if (len)
         jo_base16(j, "payload", payload, len);
      revk_info("rx", &j);
   }
   void set_uint8(const char *name, uint8_t * ptr, uint64_t flag, uint8_t val) {
      xSemaphoreTake(daikin.mutex, portMAX_DELAY);
      if (*ptr == val)
      {                         // No change
         if (daikin.control_changed & flag)
         {
            daikin.control_changed &= ~flag;
            daikin.status_changed = 1;
         }
      } else if (!(daikin.control_changed & flag))
      {                         // Changed (and not something we are tryin to set)
         *ptr = val;
         daikin.status_changed = 1;
      }
      xSemaphoreGive(daikin.mutex);
   }
   void set_float(const char *name, float *ptr, uint64_t flag, float val) {
      xSemaphoreTake(daikin.mutex, portMAX_DELAY);
      if (*ptr == val)
      {                         // No change
         if (daikin.control_changed & flag)
         {
            daikin.control_changed &= ~flag;
            daikin.status_changed = 1;
         }
      } else if (!(daikin.control_changed & flag))
      {                         // Changed (and not something we are tryin to set)
         *ptr = val;
         daikin.status_changed = 1;
      }
      xSemaphoreGive(daikin.mutex);
   }
#define set_val(name,val) set_uint8(#name,&daikin.name,CONTROL_##name,val)
#define set_temp(name,val) set_float(#name,&daikin.name,CONTROL_##name,val)
   if (cmd == 0xBA && len >= 20)
   {
      strncpy(daikin.model, (char *) payload, sizeof(daikin.model));
      daikin.status_changed = 1;
   }
   if (cmd == 0xCA && len >= 7)
   {                            // Main status settings
      set_val(online, 1);
      set_val(power, payload[0]);
      set_val(mode, payload[1]);
      set_val(compressor, payload[2]);
      set_temp(temp, payload[3] + 0.1 * payload[4]);
      set_val(fan, payload[6] & 0xF);
   }
   if (cmd == 0xCB && len >= 2)
   {

   }
   if (cmd == 0xBD && len >= 29)
   {

   }
   if (cmd == 0xBE && len >= 9)
   {

   }
#undef set_val
}

void daikin_command(uint8_t cmd, int len, uint8_t * payload)
{                               // Send a command and get response
   if (!daikin.talking)
      return;                   // Failed
   if (debug)
   {
      jo_t j = jo_object_alloc();
      jo_stringf(j, "cmd", "%02X", cmd);
      if (len)
         jo_base16(j, "payload", payload, len);
      revk_info("tx", &j);
   }
   uint8_t buf[256];
   buf[0] = 0x06;
   buf[1] = cmd;
   buf[2] = len + 6;
   buf[3] = 1;
   buf[4] = 0;
   if (len)
      memcpy(buf + 5, payload, len);
   uint8_t c = 0;
   for (int i = 0; i < 5 + len; i++)
      c += buf[i];
   buf[5 + len] = 0xFF - c;
   if (dump)
   {
      jo_t j = jo_object_alloc();
      jo_base16(j, "dump", buf, len + 6);
      revk_info("tx", &j);
   }
   uart_write_bytes(uart, buf, 6 + len);
   // Wait for reply
   len = uart_read_bytes(uart, buf, sizeof(buf), 100 / portTICK_PERIOD_MS);
   if (len <= 0)
   {
      daikin.talking = 0;
      jo_t j = jo_object_alloc();
      jo_bool(j, "timeout", 1);
      revk_error("comms", &j);
      return;
   }
   if (dump)
   {
      jo_t j = jo_object_alloc();
      jo_base16(j, "dump", buf, len);
      revk_info("rx", &j);
   }
   // Check checksum
   c = 0;
   for (int i = 0; i < len; i++)
      c += buf[i];
   if (c != 0xFF)
   {
      daikin.talking = 0;
      jo_t j = jo_object_alloc();
      jo_bool(j, "badsum", 1);
      jo_base16(j, "data", buf, len);
      revk_error("comms", &j);
      return;
   }
   // Process response
   if (buf[1] == 0xFF)
   {                            // Error report
      daikin.talking = 0;
      jo_t j = jo_object_alloc();
      jo_bool(j, "fault", 1);
      jo_base16(j, "data", buf, len);
      revk_error("comms", &j);
      return;
   }
   if (buf[0] != 0x06 || buf[1] != cmd || buf[2] != len || buf[3] != 1)
   {                            // Basic checks
      daikin.talking = 0;
      jo_t j = jo_object_alloc();
      if (buf[0] != 0x06)
         jo_bool(j, "badhead", 1);
      if (buf[1] != cmd)
         jo_bool(j, "mismatch", 1);
      if (buf[2] != len)
         jo_bool(j, "badlen", 1);
      if (buf[3] != 1)
         jo_bool(j, "badform", 1);
      jo_base16(j, "data", buf, len);
      revk_error("comms", &j);
      return;
   }
   daikin_response(cmd, len - 6, buf + 5);
}

const char *daikin_control(jo_t j)
{                               // Control settings as JSON
   jo_type_t t = jo_next(j);    // Start object
   while (t == JO_TAG)
   {
      const char *err = NULL;
      char tag[20],
       val[20];
      jo_strncpy(j, tag, sizeof(tag));
      t = jo_next(j);
      jo_strncpy(j, val, sizeof(val));
#define	b(name)		if(!strcmp(tag,#name)){if(t!=JO_TRUE&&t!=JO_FALSE)err= "Expecting boolean";else err=daikin_set_b(name,t==JO_TRUE?1:0);}
#define	t(name)		if(!strcmp(tag,#name)){if(t!=JO_NUMBER)err= "Expecting number";else err=daikin_set_t(name,jo_read_float(j));}
#define	e(name,values)	if(!strcmp(tag,#name)){if(t!=JO_STRING)err= "Expecting string";else err=daikin_set_e(name,val);}
      accontrol
#undef	b
#undef	t
#undef	e
          if (err)
      {                         // Error report
         jo_t j = jo_object_alloc();
         jo_string(j, "field", tag);
         jo_string(j, "error", err);
         revk_error("control", &j);
         return err;
      }
      t = jo_skip(j);
   }
   return NULL;
}

// --------------------------------------------------------------------------------
const char *app_callback(int client, const char *prefix, const char *target, const char *suffix, jo_t j)
{                               // MQTT app callback
   if (client || !prefix || target || strcmp(prefix, prefixcommand))
      return NULL;              // Not for us or not a command from main MQTT

   if (!strcmp(suffix, "reconnect"))
   {
      daikin.talking = 0;       // Disconnect and reconnect
      return "";
   }
   if (!suffix)
      return daikin_control(j); // General setting
   if (!strcmp(suffix, "connect") || !strcmp(suffix, "status"))
      daikin.status_changed = 1;        // Report status on connect
   jo_t s = jo_object_alloc();
   char value[20] = "";
   jo_strncpy(j, value, sizeof(value));
   // Crude commands - setting one thing
   if (!strcmp(suffix, "on"))
      jo_bool(s, "power", 1);
   if (!strcmp(suffix, "off"))
      jo_bool(s, "power", 0);
   if (!strcmp(suffix, "auto"))
      jo_string(s, "mode", "A");
   if (!strcmp(suffix, "heat"))
      jo_string(s, "mode", "H");
   if (!strcmp(suffix, "cool"))
      jo_string(s, "mode", "C");
   if (!strcmp(suffix, "dry"))
      jo_string(s, "mode", "D");
   if (!strcmp(suffix, "fan"))
      jo_string(s, "mode", "F");
   if (!strcmp(suffix, "low"))
      jo_string(s, "fan", "1");
   if (!strcmp(suffix, "medium"))
      jo_string(s, "fan", "3");
   if (!strcmp(suffix, "high"))
      jo_string(s, "fan", "5");
   if (!strcmp(suffix, "temp"))
      jo_lit(s, "temp", value);
   jo_close(s);
   jo_rewind(s);
   const char *ret = NULL;
   if (jo_next(s) == JO_TAG)
   {
      jo_rewind(s);
      ret = daikin_control(s);
      if (!ret)
         ret = "";              // OK
   }
   jo_free(&s);
   return ret;
}

void app_main()
{
   daikin.mutex = xSemaphoreCreateMutex();
   revk_boot(&app_callback);
#define io(n,d)           revk_register(#n,0,sizeof(n),&n,"- "#d,SETTING_SET|SETTING_BITFIELD);
#define b(n) revk_register(#n,0,sizeof(n),&n,NULL,SETTING_BOOLEAN);
#define u32(n,d) revk_register(#n,0,sizeof(n),&n,#d,0);
#define s8(n,d) revk_register(#n,0,sizeof(n),&n,#d,SETTING_SIGNED);
#define u8(n,d) revk_register(#n,0,sizeof(n),&n,#d,0);
#define s(n) revk_register(#n,0,0,&n,NULL,0);
   settings
#undef io
#undef u32
#undef s8
#undef u8
#undef b
#undef s
       revk_start();
   {                            // Init uart
      esp_err_t err = 0;
      // Init UART for Mobile
      uart_config_t uart_config = {
         .baud_rate = 9600,
         .data_bits = UART_DATA_8_BITS,
         .parity = UART_PARITY_EVEN,
         .stop_bits = UART_STOP_BITS_1,
         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      };
      if (!err)
         err = uart_param_config(uart, &uart_config);
      if (!err)
         err = uart_set_pin(uart, port_mask(tx), port_mask(rx), -1, -1);
      if (!err && ((tx & PORT_INV) || (rx & PORT_INV)))
         err = uart_set_line_inverse(uart, ((rx & PORT_INV) ? UART_SIGNAL_RXD_INV : 0) | ((tx & PORT_INV) ? UART_SIGNAL_TXD_INV : 0));
      if (!err)
         err = uart_driver_install(uart, 1024, 0, 0, NULL, 0);
      if (err)
      {
         jo_t j = jo_object_alloc();
         jo_string(j, "error", "Failed to uart");
         jo_int(j, "uart", uart);
         jo_int(j, "gpio", port_mask(rx));
         jo_string(j, "description", esp_err_to_name(err));
         revk_error("uart", &j);
         return;
      }
   }

   while (1)
   {                            // Main loop
      sleep(1);
      daikin.talking = 1;
      // Startup
      daikin_command(0xAA, 1, (uint8_t[])
                     {
                     0x01}
      );
      daikin_command(0xBA, 0, NULL);
      daikin_command(0xBB, 0, NULL);
      daikin.online = daikin.talking;
      daikin.status_changed = 1;
      do
      {                         // Polling loop
         //daikin_command(0xB7, 0, NULL);       // Not sure this is actually meaningful
         daikin_command(0xBD, 0, NULL);
         daikin_command(0xBE, 0, NULL);
         uint8_t ca[17] = { };
         uint8_t cb[2] = { };
         if (daikin.control_changed)
         {
            daikin.control_changed = 0; // TODO remove
            ca[0] = 2 + daikin.power;
            ca[1] = 0x10 + daikin.mode;
            if (daikin.mode >= 1 && daikin.mode <= 3)
            {                   // Temp
               ca[3] = daikin.temp;
               ca[4] = 0x80 + ((int) (daikin.temp * 10)) % 10;
            }
            if (daikin.mode == 1 || daikin.mode == 2)
               cb[0] = daikin.mode;
            else
               cb[0] = 6;
            cb[1] = 0x80 + ((daikin.fan & 7) << 4);
         }
         daikin_command(0xCA, sizeof(ca), ca);
         daikin_command(0xCB, sizeof(cb), cb);
         if (!daikin.control_changed && daikin.status_changed)
         {
            daikin.status_changed = 0;
            jo_t j = jo_object_alloc();
#define b(name)         jo_bool(j,#name,daikin.name);
#define t(name)         jo_litf(j,#name,"%.1f",daikin.name);
#define e(name,values)  if(daikin.name<sizeof(CONTROL_##name##_VALUES)-1)jo_stringf(j,#name,"%c",CONTROL_##name##_VALUES[daikin.name]);
#define s(name,len)     if(*daikin.name)jo_string(j,#name,daikin.name);
            accontrol acstatus
#undef  b
#undef  t
#undef  e
#undef  s
             revk_state("status", &j);
         }
         if (!daikin.control_changed)
            daikin.control_count = 0;
         else if (daikin.control_count++ > 100)
         {                      // Tried a lot
            // Report failed settings
            jo_t j = jo_object_alloc();
#define b(name)         if(daikin.control_changed&CONTROL_##name)jo_bool(j,#name,daikin.name);
#define t(name)         if(daikin.control_changed&CONTROL_##name)jo_litf(j,#name,"%.1f",daikin.name);
#define e(name,values)  if((daikin.control_changed&CONTROL_##name)&&daikin.name<sizeof(CONTROL_##name##_VALUES)-1)jo_stringf(j,#name,"%c",CONTROL_##name##_VALUES[daikin.name]);
            accontrol
#undef  b
#undef  t
#undef  e
                revk_error("failed-set", &j);
            daikin.control_changed = 0; // Give up on changes
         }
         revk_blink(0, 0, !daikin.online ? "P" : daikin.compressor == 1 ? "R" : "B");
      }
      while (daikin.talking);
   }
}

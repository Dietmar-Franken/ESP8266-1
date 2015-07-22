#include "ets_sys.h"
#include "stdout.h"
#include "osapi.h"
#include "mem.h"

#include "json/jsonparse.h"

#include "user_json.h"
#include "user_events.h"
#include "user_webserver.h"
#include "user_config.h"
#include "user_devices.h"

#include "user_mod_emtr.h"
#include "modules/mod_emtr.h"

LOCAL emtr_event_registers *emtr_registers = NULL;
LOCAL emtr_mode emtr_current_mode = EMTR_READ;

LOCAL const char ICACHE_FLASH_ATTR *emtr_mode_str(uint8 mode) {
	switch (mode) {
		case EMTR_READ    : return "Read";
		case EMTR_CONFIG  : return "Config";
	}
}

LOCAL void ICACHE_FLASH_ATTR emtr_event_done(emtr_packet *packet) {
	if (device_get_uart() != UART_EMTR) {
#if EMTR_DEBUG
		debug("EMTR: %s\n", DEVICE_NOT_FOUND);
#endif
		return;
	}
	
	if (emtr_registers == NULL) {
		emtr_registers = (emtr_event_registers *)os_zalloc(sizeof(emtr_event_registers));
	}
	
	emtr_parse_event(packet, emtr_registers);
}

LOCAL void emtr_start_read();

LOCAL void ICACHE_FLASH_ATTR emtr_timeout(emtr_packet *packet) {
	char response[WEBSERVER_MAX_VALUE];
	json_error(response, MOD_EMTR, TIMEOUT, NULL);
	user_event_raise(EMTR_URL, response);
	
	emtr_clear_timeout(packet);
	emtr_start_read();
}

LOCAL void ICACHE_FLASH_ATTR emtr_read_done(emtr_packet *packet) {
	LOCAL emtr_output_registers *registers = NULL;

	if (emtr_current_mode != EMTR_READ) {
		return;
	}
	
	if (registers == NULL) {
		registers = (emtr_output_registers *)os_zalloc(sizeof(emtr_output_registers));
	}
	
	emtr_parse_output(packet, registers);
	
	char event_str[20];
	os_memset(event_str, 0, sizeof(event_str));
	char status_str[20];
	os_memset(status_str, 0, sizeof(status_str));
	
	char response[WEBSERVER_MAX_RESPONSE_LEN];
	char data_str[WEBSERVER_MAX_RESPONSE_LEN];
	json_data(
		response, MOD_EMTR, OK_STR,
		json_sprintf(
			data_str,
			"\"Address\" : \"0x%04X\", "
			"\"CurrentRMS\" : %d, "
			"\"VoltageRMS\" : %d, "
			"\"ActivePower\" : %d, "
			"\"ReactivePower\" : %d, "
			"\"ApparentPower\" : %d, "
			"\"PowerFactor\" : %d, "
			"\"LineFrequency\" : %d, "
			"\"ThermistorVoltage\" : %d, "
			"\"EventFlag\" : %d, "
			"\"SystemStatus\" : \"0b%s\"",
			emtr_address(),
			registers->current_rms,
			registers->voltage_rms,
			registers->active_power,
			registers->reactive_power,
			registers->apparent_power,
			registers->power_factor,
			registers->line_frequency,
			registers->thermistor_voltage,
			registers->event_flag,
			itob(registers->system_status, status_str, 16)
		),
		NULL
	);
	
	user_event_raise(EMTR_URL, response);
	emtr_start_read();
	
	if (registers->event_flag != 0) {
		emtr_clear_event(registers->event_flag, NULL);
	}
}

LOCAL void ICACHE_FLASH_ATTR emtr_events_read() {
	if (device_get_uart() != UART_EMTR) {
#if EMTR_DEBUG
		debug("EMTR: %s\n", DEVICE_NOT_FOUND);
#endif
		return;
	}
	setTimeout(emtr_get_event, emtr_event_done, 100);
}

LOCAL void ICACHE_FLASH_ATTR emtr_start_read() {
LOCAL uint32  emtr_read_timer = 0;
	if (device_get_uart() != UART_EMTR) {
#if EMTR_DEBUG
		debug("EMTR: %s\n", DEVICE_NOT_FOUND);
#endif
		return;
	}
	
	if (emtr_current_mode != EMTR_READ) {
		return;
	}
	
	clearTimeout(emtr_read_timer);
	emtr_read_timer = setTimeout(emtr_get_output, emtr_read_done, 10);
}


void ICACHE_FLASH_ATTR emtr_handler(
	struct espconn *pConnection, 
	request_method method, 
	char *url, 
	char *data, 
	uint16 data_len, 
	uint32 content_len, 
	char *response,
	uint16 response_len
) {
	if (device_get_uart() != UART_EMTR) {
		json_error(response, MOD_EMTR, DEVICE_NOT_FOUND, NULL);
		return;
	}
	
	if (emtr_registers == NULL) {
		emtr_registers = (emtr_event_registers *)os_zalloc(sizeof(emtr_event_registers));
	}
		
	struct jsonparse_state parser;
	int type;
	emtr_mode mode = emtr_current_mode;
	
	if (method == POST && data != NULL && data_len != 0) {
		jsonparse_setup(&parser, data, data_len);
		
		while ((type = jsonparse_next(&parser)) != 0) {
			if (type == JSON_TYPE_PAIR_NAME) {
				if (jsonparse_strcmp_value(&parser, "Mode") == 0) {
					jsonparse_next(&parser);
					jsonparse_next(&parser);
					if (jsonparse_strcmp_value(&parser, "Read") == 0) {
						emtr_current_mode = EMTR_READ;
					} else if (jsonparse_strcmp_value(&parser, "Config") == 0) {
						emtr_current_mode = EMTR_CONFIG;
					}
				}
				
				if (mode == EMTR_CONFIG) {
					if (jsonparse_strcmp_value(&parser, "OverCurrentLimit") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->over_current_limit = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "OverPowerLimit") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->over_power_limit = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "OverFrequencyLimit") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->over_frequency_limit = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "UnderFrequencyLimit") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->under_frequency_limit = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "OverTemperatureLimit") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->over_temperature_limit = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "UnderTemperatureLimit") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->under_temperature_limit = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "VoltageSagLimit") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->voltage_sag_limit = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "VoltageSurgeLimit") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->voltage_surge_limit = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "OverCurrentHold") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->over_current_hold = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "OverPowerHold") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->over_power_hold = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "OverFrequencyHold") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->over_frequency_hold = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "UnderFrequencyHold") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->under_frequency_hold = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "OverTemperatureHold") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->over_temperature_hold = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "UnderTemperatureHold") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->under_temperature_hold = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "EventEnable") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->event_enable = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "EventMaskCritical") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->event_mask_critical = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "EventMaskStandard") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->event_mask_standard = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "EventTest") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->event_test = jsonparse_get_value_as_int(&parser);
					} else if (jsonparse_strcmp_value(&parser, "EventClear") == 0) {
						jsonparse_next(&parser);
						jsonparse_next(&parser);
						emtr_registers->event_clear = jsonparse_get_value_as_int(&parser);
					}
				}
			}
		}
		
		emtr_set_event(emtr_registers, NULL);
	}
	
	char data_str[WEBSERVER_MAX_RESPONSE_LEN];
	json_data(
		response, MOD_EMTR, OK_STR,
		json_sprintf(
			data_str,
			"\"Address\" : \"0x%04X\", "
			"\"Mode\" : \"%s\", "
			
			"\"CalibrationCurrent\" : %d, "
			"\"CalibrationVoltage\" : %d, "
			"\"CalibrationActivePower\" : %d, "
			"\"CalibrationReactivePower\" : %d, "
			"\"AccumulationInterval\" : %d, "
			
			"\"OverCurrentLimit\" : %d, "
			"\"OverPowerLimit\" : %d, "
			"\"OverFrequencyLimit\" : %d, "
			"\"UnderFrequencyLimit\" : %d, "
			"\"OverTemperatureLimit\" : %d, "
			"\"UnderTemperatureLimit\" : %d, "
			"\"VoltageSagLimit\" : %d, "
			"\"VoltageSurgeLimit\" : %d, "
			"\"OverCurrentHold\" : %d, "
			"\"OverPowerHold\" : %d, "
			"\"OverFrequencyHold\" : %d, "
			"\"UnderFrequencyHold\" : %d, "
			"\"OverTemperatureHold\" : %d, "
			"\"UnderTemperatureHold\" : %d, "
			"\"EventEnable\" : %d, "
			"\"EventMaskCritical\" : %d, "
			"\"EventMaskStandard\" : %d, "
			"\"EventTest\" : %d, "
			"\"EventClear\" : %d",
			emtr_address(),
			emtr_mode_str(emtr_current_mode),
			
			emtr_registers->calibration_current,
			emtr_registers->calibration_voltage,
			emtr_registers->calibration_active_power,
			emtr_registers->calibration_reactive_power,
			emtr_registers->accumulation_interval,
			
			emtr_registers->over_current_limit,
			emtr_registers->over_power_limit,
			emtr_registers->over_frequency_limit,
			emtr_registers->under_frequency_limit,
			emtr_registers->over_temperature_limit,
			emtr_registers->under_temperature_limit,
			emtr_registers->voltage_sag_limit,
			emtr_registers->voltage_surge_limit,
			emtr_registers->over_current_hold,
			emtr_registers->over_power_hold,
			emtr_registers->over_frequency_hold,
			emtr_registers->under_frequency_hold,
			emtr_registers->over_temperature_hold,
			emtr_registers->under_temperature_hold,
			emtr_registers->event_enable,
			emtr_registers->event_mask_critical,
			emtr_registers->event_mask_standard,
			emtr_registers->event_test,
			emtr_registers->event_clear
		),
		NULL
	);
	
	setTimeout(emtr_events_read, NULL, 1500);
	emtr_start_read();
}

void ICACHE_FLASH_ATTR mod_emtr_init() {
	emtr_set_timeout_callback(emtr_timeout);
	webserver_register_handler_callback(EMTR_URL, emtr_handler);
	device_register(UART, 0, EMTR_URL, emtr_init);
	
	setTimeout(emtr_events_read, NULL, 1500);
	setTimeout(emtr_start_read, NULL, 2000);
}
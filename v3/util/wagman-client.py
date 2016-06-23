#!/usr/bin/env python

import sys
from serial import Serial


header_prefix = '<<<-'
footer_prefix = '->>>'
wagman_device = '/dev/waggle_sysmon'


usage_dict={
    'start'     : ['start <portnum> - starts device on portnum'],
    'stop'      : ['stop <portnum> - stops device on portnum'],
    'stop!'     : ['stop! <portnum> - immediately kills power to device on portnum'],
    'info'      : ['info - prints some system info'],
    'eedump'    : ['edump - prints a hex dump of all EEPROM'],
    'date'      : ['date - shows rtc date and time', 
                    'date <year> <month> <day> <hour> <minute> <second> - sets rtc date and time'],
    'cu'        : ['cu - current usage'],
    'hb'        : ['hb - last heartbeat times'],
    'therm'     : ['therm - thermistor values (though none are connected right now)']
    }



def wagman_client(args):
    serial = Serial(wagman_device, 115200)

    command = ' '.join(args)
    serial.write(command.encode('ascii'))
    serial.write(b'\n')

    # header
    header = serial.readline().decode().strip()

    if header.startswith(header_prefix):
        # TODO parse header line
        pass
    else:
        serial.close()
        raise Exception('header not found')


    while (1):
        line = serial.readline().decode().strip()
    
        if line.startswith(footer_prefix):
            break
        
        yield line
    
    serial.close()   
    


if __name__ == "__main__":

    if sys.argv[1] == 'help' or sys.argv[1] == '?':
        for line in wagman_client(['help']):
            if line in usage_dict:
                print "\n".join(usage_dict[line])
                
        sys.exit(0)

    for line in wagman_client(sys.argv[1:]):
        print line






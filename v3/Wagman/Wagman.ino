#include <avr/wdt.h>
#include <EEPROM.h>
#include "Wagman.h"
#include "Record.h"
#include "Device.h"
#include "Logger.h"
#include "Error.h"
#include "MCP79412RTC.h"

/*
 * TODO Speedup communications. Not bad now, but could be faster?
 * TODO Add error detection to communications. Can still easily write a front end to talk using protocol.
 * TODO Cleanup, really think about communication pathway. Does this allow us to reduce anything from the core?
 * TODO Add persistant event logging on using system hardware so we can trace what happened during a device failure.
 * TODO Check that system current reading is correct.
 * TODO Move towards errors aware API.
 * TODO Go a little easier on faults / if gn locks up system, don't want to totally kill.
 * TODO In the longer term, clean up the device abstraction. I wrote that initially before I realized
 * that we'd have things like the coresense using a power port or different thermistors not related to
 * a particular device. What we have now is ok, but should be improved for future clarity.
 * TODO Double check the media is being output correctly.
 * TODO Get a sense of power usage of the system.
 * TODO Command for resetting PMEM.
 */

static const byte DEVICE_COUNT = 5;
static const byte BUFFER_SIZE = 80;
static const byte MAX_ARGC = 8;

byte bootflags = 0;
bool shouldResetSystem = false;
bool logging = true;
byte deviceWantsStart = 255;

Device devices[DEVICE_COUNT];

static Timer startTimer;
static Timer statusTimer;

static char buffer[BUFFER_SIZE];
static byte bufferSize = 0;

time_t setupTime;

bool isspace(char c);
bool isgraph(char c);

struct Command {
    const char *name;
    byte (*func)(byte, const char **);
};

byte commandStart(byte argc, const char **argv);
byte commandStop(byte argc, const char **argv);
byte commandKill(byte argc, const char **argv);
byte commandReset(byte argc, const char **argv);
byte commandPing(byte argc, const char **argv);
byte commandID(byte argc, const char **argv);
byte commandEEDump(byte argc, const char **argv);
byte commandDate(byte argc, const char **argv);
byte commandCurrent(byte argc, const char **argv);
byte commandHeartbeat(byte argc, const char **argv);
byte commandThermistor(byte argc, const char **argv);
byte commandEnvironment(byte argc, const char **argv);
byte commandBootMedia(byte argc, const char **argv);
byte commandFailCount(byte argc, const char **argv);
byte commandLog(byte argc, const char **argv);
byte commandBootFlags(byte argc, const char **argv);
byte commandUptime(byte argc, const char **argv);
byte commandEnable(byte argc, const char **argv);
byte commandWatch(byte argc, const char **argv);
byte commandResetEEPROM(byte argc, const char **argv);

Command commands[] = {
    { "ping", commandPing },
    { "start", commandStart },
    { "stop", commandStop },
    { "stop!", commandKill },
    { "reset", commandReset },
    { "id", commandID },
    { "cu", commandCurrent },
    { "hb", commandHeartbeat },
    { "env", commandEnvironment},
    { "bs", commandBootMedia },
    { "th", commandThermistor },
    { "date", commandDate },
    { "eedump", commandEEDump },
    { "bf", commandBootFlags },
    { "fc", commandFailCount },
    { "up", commandUptime },
    { "en", commandEnable },
    { "watch", commandWatch },
    { "log", commandLog },
    { "eereset", commandResetEEPROM },
    { NULL, NULL },
};

byte commandPing(byte argc, const char **argv)
{
    if (argc != 2)
        return ERROR_INVALID_ARGC;

    byte port = atoi(argv[1]);

    if (!Wagman::validPort(port))
        return ERROR_INVALID_PORT;

    devices[port].sendExternalHeartbeat();

    return 0;
}

byte commandStart(byte argc, const char **argv)
{
    if (argc != 2)
        return ERROR_INVALID_ARGC;

    byte port = atoi(argv[1]);

    if (!Wagman::validPort(port))
        return ERROR_INVALID_PORT;

    deviceWantsStart = port;

    return 0;
}

byte commandStop(byte argc, const char **argv)
{
    if (argc != 2)
        return ERROR_INVALID_ARGC;

    byte port = atoi(argv[1]);

    if (!Wagman::validPort(port))
        return ERROR_INVALID_PORT;

    devices[port].stop();

    return 0;
}

byte commandKill(byte argc, const char **argv)
{
    if (argc != 2)
        return ERROR_INVALID_ARGC;

    byte port = atoi(argv[1]);

    if (!Wagman::validPort(port))
        return ERROR_INVALID_PORT;

    devices[port].kill();

    return 0;
}

byte commandReset(byte argc, const char **argv)
{
    shouldResetSystem = true;
    return 0;
}

byte commandID(__attribute__ ((unused)) byte argc, __attribute__ ((unused)) const char **argv)
{
    byte id[8];

    RTC.idRead(id);

    for (byte i = 0; i < 8; i++) {
        Serial.print(id[i] & 0x0F, HEX);
        Serial.print(id[i] >> 8, HEX);
    }

    Serial.println();

    return 0;
}

byte commandEEDump(__attribute__ ((unused)) byte argc, __attribute__ ((unused)) const char **argv)
{
    for (unsigned int i = 0; i < 1024; i++) {
        byte value = EEPROM.read(i);

        Serial.print((value >> 4) & 0x0F, HEX);
        Serial.print(value & 0x0F, HEX);
        Serial.print(' ');

        if (i % 4 == 3)
            Serial.print(' ');

        if (i % 32 == 31)
            Serial.println();
    }

    return 0;
}

byte commandDate(byte argc, const char **argv)
{
    if (argc != 1 && argc != 7)
        return ERROR_INVALID_ARGC;

    if (argc == 1) {
        tmElements_t tm;

        RTC.read(tm);

        Serial.print(tm.Year + 1970);
        Serial.print(' ');
        Serial.print(tm.Month);
        Serial.print(' ');
        Serial.print(tm.Day);
        Serial.print(' ');
        Serial.print(tm.Hour);
        Serial.print(' ');
        Serial.print(tm.Minute);
        Serial.print(' ');
        Serial.print(tm.Second);
        Serial.println();
    } else if (argc == 7) {
        tmElements_t tm;

        tm.Year = atoi(argv[1]) - 1970;
        tm.Month = atoi(argv[2]);
        tm.Day = atoi(argv[3]);
        tm.Hour = atoi(argv[4]);
        tm.Minute = atoi(argv[5]);
        tm.Second = atoi(argv[6]);

        RTC.set(makeTime(tm));
    }

    return 0;
}

byte commandCurrent(__attribute__ ((unused)) byte argc, __attribute__ ((unused)) const char **argv)
{
    Serial.print(Wagman::getCurrent());

    for (byte i = 0; i < DEVICE_COUNT; i++) {
        Serial.print(' ');
        Serial.print(Wagman::getCurrent(i));
    }

    Serial.println();

    return 0;
}

byte commandHeartbeat(__attribute__ ((unused)) byte argc, __attribute__ ((unused)) const char **argv)
{
    for (byte i = 0; i < DEVICE_COUNT; i++) {
        Serial.println(devices[i].timeSinceHeartbeat());
    }

    return 0;
}

byte commandFailCount(__attribute__ ((unused)) byte argc, __attribute__ ((unused)) const char **argv)
{
    for (byte i = 0; i < DEVICE_COUNT; i++) {
        Serial.println(Record::getBootFailures(i));
    }

    return 0;
}

byte commandThermistor(__attribute__ ((unused)) byte argc, __attribute__ ((unused)) const char **argv)
{
    for (byte i = 0; i < DEVICE_COUNT; i++) {
        Serial.println(Wagman::getThermistor(i));
    }

    return 0;
}

byte commandEnvironment(__attribute__ ((unused)) byte argc, __attribute__ ((unused)) const char **argv)
{
    Serial.print("temperature ");
    Serial.println(Wagman::getTemperature());

    Serial.print("humidity ");
    Serial.println(Wagman::getHumidity());

    return 0;
}

byte commandBootMedia(byte argc, const char **argv)
{
    if (argc != 2 && argc != 3)
        return ERROR_INVALID_ARGC;

    byte index = atoi(argv[1]);

    if (!Wagman::validPort(index))
        return ERROR_INVALID_PORT;

    if (argc == 3) {
        if (strcmp(argv[2], "sd") == 0) {
            devices[index].shouldForceBootMedia = true;
            devices[index].forceBootMedia = MEDIA_SD;
            Serial.println("set sd");
        } else if (strcmp(argv[2], "emmc") == 0) {
            devices[index].shouldForceBootMedia = true;
            devices[index].forceBootMedia = MEDIA_EMMC;
            Serial.println("set emmc");
        } else {
            Serial.println("invalid media");
        }
    } else if (argc == 2) {
        byte bootMedia = devices[index].getBootMedia();

        if (bootMedia == MEDIA_SD) {
            Serial.println("sd");
        } else if (bootMedia == MEDIA_EMMC) {
            Serial.println("emmc");
        } else {
            Serial.println("invalid media");
        }
    }

    return 0;
}

byte commandLog(byte argc, const char **argv)
{
    if (argc == 2) {
        if (strcmp(argv[1], "on") == 0) {
            logging = true;
        } else if (strcmp(argv[1], "off") == 0) {
            logging = false;
        }
    }

    return 0;
}

byte commandBootFlags(__attribute__ ((unused)) byte argc, __attribute__ ((unused)) const char **argv)
{
    if (bootflags & _BV(WDRF))
        Serial.println("WDRF");
    if (bootflags & _BV(BORF))
        Serial.println("BORF");
    if (bootflags & _BV(EXTRF))
        Serial.println("EXTRF");
    if (bootflags & _BV(PORF))
        Serial.println("PORF");
    return 0;
}

byte commandUptime(__attribute__ ((unused)) byte argc, __attribute__ ((unused)) const char **argv)
{
    time_t time;
    Wagman::getTime(time);
    Serial.println(time - setupTime);
    return 0;
}

byte commandEnable(byte argc, const char **argv)
{
    if (argc == 2) {
        byte index = atoi(argv[1]);
        if (Wagman::validPort(index)) {
            Serial.println(Record::getDeviceEnabled(index));
        }
    } else if (argc == 3) {
        byte index = atoi(argv[1]);
        if (Wagman::validPort(index)) {
            Record::setDeviceEnabled(index, strcmp(argv[2], "1") == 0);
        }
    }

    return 0;
}

byte commandWatch(byte argc, const char **argv)
{
//    if (argc != 4)
//        return ERROR_USAGE;

    if (argc == 4) {
        byte index = atoi(argv[1]);
        if (Wagman::validPort(index)) {
            bool mode = strcmp(argv[3], "t") == 0;

            if (strcmp(argv[2], "hb") == 0) {
                devices[index].watchHeartbeat = mode;
            } else if (strcmp(argv[2], "cu")) {
                devices[index].watchCurrent = mode;
            }
        }
    }

    return 0;
}

void executeCommand(const char *sid, byte argc, const char **argv)
{
    byte error = 0;
    byte (*func)(byte, const char **) = NULL;

    // search for command and execute if found
    for (byte i = 0; commands[i].name != NULL; i++) {
        if (strcmp(commands[i].name, argv[0]) == 0) {
            func = commands[i].func;
            break;
        }
    }

    // marks the beginning of a response packet.
    Serial.print("<<<- sid=");
    Serial.print(sid);
    Serial.print(' ');
    Serial.println(argv[0]);

    if (func != NULL) {
        error = func(argc, argv);
    } else {
        Serial.println("command not found");
    }

    // marks the end of a response packet.
    Serial.println("->>>");
}

byte commandResetEEPROM(byte argc, const char **argv)
{
    Record::init();
    return 0;
}

// Assumes ASCII encoding.
bool isspace(char c) {
    return c == ' ' || c == '\t';
}

// Assumes ASCII encoding.
bool isgraph(char c) {
    return '!' <= c && c <= '~';
}

void processCommand()
{
    byte argc = 0;
    const char *argv[MAX_ARGC];

    char *s = buffer;

    while (argc < MAX_ARGC) {

        // skip whitespace
        while (isspace(*s)) {
            s++;
        }

        // check if next character is an argument
        if (isgraph(*s)) {

            // mark start of argument.
            argv[argc++] = s;

            // scan remainder of argument.
            while (isgraph(*s)) {
                s++;
            }
        }

        // end of buffer?
        if (*s == '\0') {
            break;
        }

        // mark end of argument.
        *s++ = '\0';
    }

    if (argc > 0) {
        // this sid case was suggested by wolfgang to help identify the source of certain messages.
        if (argv[0][0] == '@') {
            executeCommand(argv[0] + 1, argc - 1, argv + 1);
        } else {
            executeCommand("0", argc, argv);
        }
    }
}

void processCommands()
{
    byte dataread = 0; // using this instead of a timer to reduce delay in processing byte stream

    while (Serial.available() > 0 && dataread < 240) {
        char c = Serial.read();

        buffer[bufferSize++] = c;
        dataread++;

        if (bufferSize >= BUFFER_SIZE) { // exceeds buffer! dangerous!
            bufferSize = 0;

            // flush remainder of line.
            while (Serial.available() > 0 && dataread < 240) {
                if (Serial.read() == '\n')
                    break;
                dataread++;
            }
        } else if (c == '\n') {
            buffer[bufferSize] = '\0';
            bufferSize = 0;
            processCommand();
        }
    }
}

void setup()
{
    bootflags = MCUSR;
    MCUSR = 0;

    wdt_enable(WDTO_8S);
    wdt_reset();

    Wagman::init();

    wdt_reset();

    // if we started from a power on or brown out
    if (bootflags & _BV(PORF) || bootflags & _BV(BORF)) {
        // kill all of the devices onboard
        for (byte i = 0; i < 5; i++) {
            Wagman::setRelay(i, false);
            delay(100);
        }
    }

    wdt_reset();

    for (byte i = 0; i < 8; i++) {
        Wagman::setLED(0, true);
        delay(100);
        Wagman::setLED(1, true);
        delay(100);
        Wagman::setLED(0, false);
        delay(100);
        Wagman::setLED(1, false);
        delay(100);
    }

    wdt_reset();
    Serial.begin(57600);

    delay(2000);

    if (!Record::initialized()) {
        Logger::begin("init");
        Logger::log("record init");
        Logger::end();

        wdt_reset();
        Record::init();
    }

    devices[0].name = "nc";
    devices[0].port = 0;
    devices[0].bootSelector = 0;
    devices[0].primaryMedia = MEDIA_SD;
    devices[0].secondaryMedia = MEDIA_EMMC;
    devices[0].watchHeartbeat = true;
    devices[0].watchCurrent = true;

    devices[1].name = "gn";
    devices[1].port = 1;
    devices[1].bootSelector = 1;
    devices[1].primaryMedia = MEDIA_SD;
    devices[1].secondaryMedia = MEDIA_EMMC;
    devices[1].watchHeartbeat = true;
    devices[1].watchCurrent = true;

    devices[2].name = "cs";
    devices[2].port = 2;
    devices[2].primaryMedia = MEDIA_SD;
    devices[2].secondaryMedia = MEDIA_EMMC;
    devices[2].watchHeartbeat = false;
    devices[2].watchCurrent = false;

    devices[3].name = "x1";
    devices[3].port = 3;
    devices[3].watchHeartbeat = false;
    devices[3].watchCurrent = false;

    devices[4].name = "x2";
    devices[4].port = 4;
    devices[4].watchHeartbeat = false;
    devices[4].watchCurrent = false;

    Wagman::getTime(setupTime);
    Record::setLastBootTime(setupTime);
    Record::incrementBootCount();

    // Wait to see if node controller heartbeat was detected.
    for (byte i = 0; i < 8; i++) {
        wdt_reset();

        Wagman::setLED(0, true);
        delay(250);
        Wagman::setLED(1, true);
        delay(250);
        Wagman::setLED(1, false);
        delay(250);
        Wagman::setLED(0, false);
        delay(250);

        if (heartbeatCounters[0] >= 4 || Wagman::getCurrent(0) > 120) {
            // actually change the device state to reflect what we think!
            break;
        }
    }

    wdt_reset();

    if (bootflags & _BV(WDRF)) {
        for (byte i = 0; i < 5; i++) {
            byte state = Record::getRelayState(i);

            if (state == RELAY_TURNING_ON || state == RELAY_TURNING_OFF) {
                Record::setRelayState(i, RELAY_OFF);
            }
        }

        Logger::end();
    }

    if (bootflags & _BV(PORF) || bootflags & _BV(BORF)) {
        Logger::begin("init");

        if (bootflags & _BV(PORF))
            Logger::log("porf ");

        if (bootflags & _BV(BORF))
            Logger::log("borf ");

        for (byte i = 0; i < 5; i++) {
            byte state = Record::getRelayState(i);

            if (state == RELAY_TURNING_ON || state == RELAY_TURNING_OFF) {
                Record::setDeviceEnabled(i, false);
                Record::setRelayState(i, RELAY_OFF);

                Logger::log(" relay fault ");
                Logger::log(i);
            }
        }

        Logger::end();
    }

    if (bootflags == 0) {
        Logger::begin("init");
        Logger::log("ok");
        Logger::end();
    }

    for (byte i = 0; i < DEVICE_COUNT; i++) {
        devices[i].init();
    }

    // set the node controller as starting device
    deviceWantsStart = 0;
    shouldResetSystem = false; // used to reset Wagman in main loop
    bufferSize = 0;
    startTimer.reset();
    statusTimer.reset();
}

void startNextDevice()
{
    // if we've asked for a specific device, start that device.
    if (Wagman::validPort(deviceWantsStart)) {
        startTimer.reset();
        devices[deviceWantsStart].start();
        deviceWantsStart = 255;
        return;
    }

    if (startTimer.exceeds(30000)) {
        startTimer.reset();

        for (byte i = 0; i < DEVICE_COUNT; i++) {
            if (devices[i].canStart()) { // include !started in canStart() call.
                devices[i].start();
                break;
            }
        }
    }
}

void loop()
{
    // ensure that the watchdog is always enabled
    wdt_enable(WDTO_8S);
    wdt_reset();

    startNextDevice();

    for (byte i = 0; i < DEVICE_COUNT; i++) {
        devices[i].update();
    }

    wdt_reset();
    processCommands();

    if (statusTimer.exceeds(30000)) {
        statusTimer.reset();

        wdt_reset();
        logStatus();
    }

    if (shouldResetSystem) {
        resetSystem();
    }

    Wagman::toggleLED(0);
}

void resetSystem()
{
    // ensure that watchdog is set!
    wdt_enable(WDTO_8S);
    wdt_reset();

    // watchdog will reset in a few seconds.
    for (;;) {
        for (byte i = 0; i < 5; i++) {
            Wagman::setLED(0, true);
            Wagman::setLED(1, true);
            delay(80);
            Wagman::setLED(0, false);
            Wagman::setLED(1, false);
            delay(80);
        }

        Wagman::setLED(0, true);
        Wagman::setLED(1, true);
        delay(200);
        Wagman::setLED(0, false);
        Wagman::setLED(1, false);
        delay(200);
    }
}

void logStatus()
{
    byte id[8];
    RTC.idRead(id);

    Logger::begin("id");

    for (byte i = 0; i < 8; i++) {
        Logger::logHex(id[i] & 0x0F);
        Logger::logHex(id[i] >> 8);
    }

    Logger::end();

    delay(50);

    tmElements_t tm;
    RTC.read(tm);

    Logger::begin("date");
    Logger::log(tm.Year + 1970);
    Logger::log(' ');
    Logger::log(tm.Month);
    Logger::log(' ');
    Logger::log(tm.Day);
    Logger::log(' ');
    Logger::log(tm.Hour);
    Logger::log(' ');
    Logger::log(tm.Minute);
    Logger::log(' ');
    Logger::log(tm.Second);
    Logger::end();

    delay(50);

    Logger::begin("cu");

    Logger::log(Wagman::getCurrent());

    for (byte i = 0; i < 5; i++) {
        Logger::log(' ');
        Logger::log(Wagman::getCurrent(i));
    }

    Logger::end();

    delay(50);

    Logger::begin("th");

    for (byte i = 0; i < 5; i++) {
        Logger::log(Wagman::getThermistor(i));
        Logger::log(' ');
    }

    Logger::end();

    delay(50);

    Logger::begin("env");
    Logger::log(Wagman::getTemperature());
    Logger::log(' ');
    Logger::log(Wagman::getHumidity());
    Logger::end();

    delay(50);

    Logger::begin("fails");

    for (byte i = 0; i < 5; i++) {
        Logger::log(Record::getBootFailures(i));
        Logger::log(' ');
    }

    Logger::end();

    delay(50);

    Logger::begin("enabled");

    for (byte i = 0; i < 5; i++) {
        Logger::log(Record::getDeviceEnabled(i));
        Logger::log(' ');
    }

    Logger::end();

    delay(50);

    Logger::begin("media");

    for (byte i = 0; i < 5; i++) {
        byte bootMedia = devices[i].getBootMedia();

        if (bootMedia == MEDIA_SD) {
            Logger::log("sd ");
        } else if (bootMedia == MEDIA_EMMC) {
            Logger::log("emmc ");
        } else {
            Logger::log("? ");
        }
    }

    Logger::end();
}

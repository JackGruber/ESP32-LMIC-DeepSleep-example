#include <arduino.h>
#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>
#include <ttn_credentials.h>

bool GOTO_DEEPSLEEP = false;

// rename ttn_credentials.h.example to ttn_credentials.h and add you keys
static const u1_t PROGMEM APPEUI[8] = TTN_APPEUI;
static const u1_t PROGMEM DEVEUI[8] = TTN_DEVEUI;
static const u1_t PROGMEM APPKEY[16] = TTN_APPKEY;
void os_getArtEui(u1_t *buf) { memcpy_P(buf, APPEUI, 8); }
void os_getDevEui(u1_t *buf) { memcpy_P(buf, DEVEUI, 8); }
void os_getDevKey(u1_t *buf) { memcpy_P(buf, APPKEY, 16); }

static uint8_t mydata[] = "Hello, world!";
static osjob_t sendjob;

// Schedule TX every this many seconds
// Respect Fair Access Policy and Maximum Duty Cycle!
// https://www.thethingsnetwork.org/docs/lorawan/duty-cycle.html
// https://www.loratools.nl/#/airtime
const unsigned TX_INTERVAL = 300;

// Saves the LMIC structure during DeepSleep
RTC_DATA_ATTR lmic_t RTC_LMIC;

#define PIN_LMIC_NSS 18
#define PIN_LMIC_RST 14
#define PIN_LMIC_DIO0 26
#define PIN_LMIC_DIO1 33
#define PIN_LMIC_DIO2 32

// Pin mapping
const lmic_pinmap lmic_pins = {
    .nss = PIN_LMIC_NSS,
    .rxtx = LMIC_UNUSED_PIN,
    .rst = PIN_LMIC_RST,
    .dio = {PIN_LMIC_DIO0, PIN_LMIC_DIO1, PIN_LMIC_DIO2},
};

void onEvent(ev_t ev)
{
    Serial.print(os_getTime());
    Serial.print(": ");
    switch (ev)
    {
    case EV_SCAN_TIMEOUT:
        Serial.println(F("EV_SCAN_TIMEOUT"));
        break;
    case EV_BEACON_FOUND:
        Serial.println(F("EV_BEACON_FOUND"));
        break;
    case EV_BEACON_MISSED:
        Serial.println(F("EV_BEACON_MISSED"));
        break;
    case EV_BEACON_TRACKED:
        Serial.println(F("EV_BEACON_TRACKED"));
        break;
    case EV_JOINING:
        Serial.println(F("EV_JOINING"));
        break;
    case EV_JOINED:
        Serial.println(F("EV_JOINED"));
        {
            u4_t netid = 0;
            devaddr_t devaddr = 0;
            u1_t nwkKey[16];
            u1_t artKey[16];
            LMIC_getSessionKeys(&netid, &devaddr, nwkKey, artKey);
            Serial.print("netid: ");
            Serial.println(netid, DEC);
            Serial.print("devaddr: ");
            Serial.println(devaddr, HEX);
            Serial.print("artKey: ");
            for (size_t i = 0; i < sizeof(artKey); ++i)
            {
                Serial.print(artKey[i], HEX);
            }
            Serial.println("");
            Serial.print("nwkKey: ");
            for (size_t i = 0; i < sizeof(nwkKey); ++i)
            {
                Serial.print(nwkKey[i], HEX);
            }
            Serial.println("");
        }
        // Disable link check validation (automatically enabled
        // during join, but because slow data rates change max TX
        // size, we don't use it in this example.
        LMIC_setLinkCheckMode(0);
        break;
    /*
        || This event is defined but not used in the code. No
        || point in wasting codespace on it.
        ||
        || case EV_RFU1:
        ||     Serial.println(F("EV_RFU1"));
        ||     break;
        */
    case EV_JOIN_FAILED:
        Serial.println(F("EV_JOIN_FAILED"));
        break;
    case EV_REJOIN_FAILED:
        Serial.println(F("EV_REJOIN_FAILED"));
        break;
    case EV_TXCOMPLETE:
        Serial.println(F("EV_TXCOMPLETE (includes waiting for RX windows)"));
        if (LMIC.txrxFlags & TXRX_ACK)
            Serial.println(F("Received ack"));
        if (LMIC.dataLen)
        {
            Serial.print(F("Received "));
            Serial.print(LMIC.dataLen);
            Serial.println(F(" bytes of payload"));
        }
        GOTO_DEEPSLEEP = true;
        break;
    case EV_LOST_TSYNC:
        Serial.println(F("EV_LOST_TSYNC"));
        break;
    case EV_RESET:
        Serial.println(F("EV_RESET"));
        break;
    case EV_RXCOMPLETE:
        // data received in ping slot
        Serial.println(F("EV_RXCOMPLETE"));
        break;
    case EV_LINK_DEAD:
        Serial.println(F("EV_LINK_DEAD"));
        break;
    case EV_LINK_ALIVE:
        Serial.println(F("EV_LINK_ALIVE"));
        break;
    /*
        || This event is defined but not used in the code. No
        || point in wasting codespace on it.
        ||
        || case EV_SCAN_FOUND:
        ||    Serial.println(F("EV_SCAN_FOUND"));
        ||    break;
        */
    case EV_TXSTART:
        Serial.println(F("EV_TXSTART"));
        break;
    case EV_TXCANCELED:
        Serial.println(F("EV_TXCANCELED"));
        break;
    case EV_RXSTART:
        /* do not print anything -- it wrecks timing */
        break;
    case EV_JOIN_TXCOMPLETE:
        Serial.println(F("EV_JOIN_TXCOMPLETE: no JoinAccept"));
        break;
    default:
        Serial.print(F("Unknown event: "));
        Serial.println((unsigned)ev);
        break;
    }
}

void do_send(osjob_t *j)
{
    // Check if there is not a current TX/RX job running
    if (LMIC.opmode & OP_TXRXPEND)
    {
        Serial.println(F("OP_TXRXPEND, not sending"));
    }
    else
    {
        // Prepare upstream data transmission at the next possible time.
        LMIC_setTxData2(1, mydata, sizeof(mydata) - 1, 0);
        Serial.println(F("Packet queued"));
    }
    // Next TX is scheduled after TX_COMPLETE event.
}

void SaveLMICToRTC()
{
    RTC_LMIC = LMIC;
}

void LoadLMICFromRTC()
{
    LMIC = RTC_LMIC;

    // ESP32 can'*'t track millis during DeepSleep and no option to advanced millis after DeepSleep.
    // Therefore reset DutyCyles
    // Respect Fair Access Policy and Maximum Duty Cycle
    // https://www.thethingsnetwork.org/docs/lorawan/duty-cycle.html
    // https://www.loratools.nl/#/airtime
    for (u1_t bi = 0; bi < MAX_BANDS; bi++)
    {
        LMIC.bands[bi].avail = 0;
    }
    LMIC.globalDutyAvail = 0;
}

void GoDeepSleep()
{
    Serial.println(F("Go DeepSleep"));
    Serial.flush();
    esp_sleep_enable_timer_wakeup(TX_INTERVAL * 1000000);
    esp_deep_sleep_start();
}

void setup()
{
    Serial.begin(115200);

    Serial.println(F("Starting DeepSleep test"));

    // LMIC init
    os_init();

    // Reset the MAC state. Session and pending data transfers will be discarded.
    LMIC_reset();

    if (RTC_LMIC.seqnoUp != 0)
    {
        LoadLMICFromRTC();
    }

    // Start job (sending automatically starts OTAA too)
    do_send(&sendjob);
}

void loop()
{
    static unsigned long lastPrintTime = 0;

    os_runloop_once();

    if (!os_queryTimeCriticalJobs(ms2osticksRound((TX_INTERVAL * 1000))) && GOTO_DEEPSLEEP == true)
    {
        SaveLMICToRTC();
        GoDeepSleep();
    }
    else if (lastPrintTime + 1000 < millis())
    {
        Serial.println(F("Cannot sleep"));
        lastPrintTime = millis();
    }
}

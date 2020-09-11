/****************************************************************************
 Module
   MainService.c

 Description
   This is a main state machine for the device.

 Notes
****************************************************************************/
/*----------------------------- Include Files -----------------------------*/
#include "MainService.h"
#include "ePaperDriver.h"
#include "ES_framework.h"
#include "ES_Timers.h"
#include "CloudService.h"
#include "WiFi.h"
#include "IAQ_util.h"
#include "driver/adc.h"
#include <esp_wifi.h>
#include <esp_bt.h>

/*----------------------------- Module Defines ----------------------------*/
#define ALL_SENSORS_READ 0x07
#define BAT_LOW_THRES 3500  // voltage at which to go into hibernation mode
#define AUTO_MODE_TIMER_LEN 300000UL  // Time in ms
#define STREAM_MODE_TIMER_LEN 6000U  // Time in ms
#define WARMUP_TIMER_LEN 40000UL  //186000UL  // Time in ms
#define WIFI_TIMEOUT_LEN  36000UL  // ms
#define CLOUD_COUNTER_LEN 10  // Cloud updates every this many screen refreshes   

typedef enum
{
  CO2Idx = 0,
  HPMIdx,
  SVM30Idx
}sensorIdx_t;

typedef enum
{
  START_STATE,
  AUTO_STATE,
  STREAM_STATE,
  STREAM_CLOUD_STATE
}statusState_t; 


/*---------------------------- Module Functions ---------------------------*/
void initPins();
void sensorsPwrEnable(bool turnOn);
void setFlagBit(sensorIdx_t sensorBitIdx);
void shutdownIAQ(bool timedShtdwn);
void shutdownBat();
void changeSensorsIAQMode(IAQmode_t currIAQMode);
void startSensorsSM();

/*---------------------------- Module Variables ---------------------------*/
static uint8_t MyPriority;
static uint8_t sensorReads_flag = 0x00; 
static IAQsensorVals_t sensorReads = {.eCO2=-1, .tVOC=-1, .PM25=-1, .PM10=-1, .CO2=-1, .temp=-1, .rh=-1}; 
static statusState_t currSMState = START_STATE; 

/*------------------------------ Module Code ------------------------------*/
/****************************************************************************
 Function
     InitMainService

 Parameters
     uint8_t : the priorty of this service

 Returns
     bool, false if error in initialization, true otherwise

 Description
     Saves away the priority, and does any
     other required initialization for this service
 Notes
****************************************************************************/
bool InitMainService(uint8_t Priority)
{
  ES_Event_t ThisEvent;
  MyPriority = Priority;
  
  if(EventCheckerBat())
  {
    shutdownBat(); 
  }
  initPins(); 
  initePaper();
  adc_power_on();
  btStop();  // Make sure bluetooth is off

  setenv("TZ", "PST8PDT", 1); // set the correct timezone for time.h  
  tzset();

  char str[20]; 
  if(isTimeSynced())
    getCurrTime(str, 20); 
  else 
    strcpy(str, "Time not synced");

  printf("Start time: %s\n", str); 

  // post the initial transition event
  ThisEvent.EventType = ES_INIT;
  ThisEvent.ServiceNum = Priority;
  if (ES_PostToService(ThisEvent) == true)
  {
    return true;
  }
  else
  {
    return false;
  }
}

/****************************************************************************
 Function
     PostMainService

 Parameters
     EF_Event_t ThisEvent ,the event to post to the queue

 Returns
     bool false if the Enqueue operation failed, true otherwise

 Description
     Posts an event to this state machine's queue
 Notes
****************************************************************************/
bool PostMainService(ES_Event_t ThisEvent)
{
  ThisEvent.ServiceNum = MyPriority; 
  return ES_PostToService(ThisEvent);
}

/****************************************************************************
 Function
    RunMainService

 Parameters
   ES_Event_t : the event to process

 Returns
   ES_Event, ES_NO_EVENT if no error ES_ERROR otherwise

 Description
   main state machine for the device
 Notes
****************************************************************************/
ES_Event_t RunMainService(ES_Event_t ThisEvent)
{
  ES_Event_t ReturnEvent;
  ReturnEvent.EventType = ES_NO_EVENT; // assume no errors
  static uint8_t cloudUpdateCounter = 1; 

  if(ThisEvent.EventType == LOW_BAT_EVENT)
  {
    shutdownBat();     
  }
  if(ThisEvent.EventType == ES_SW_BUTTON_PRESS && ThisEvent.EventParam == LONG_BT_PRESS)
  {
    ePaperChangeMode(NO_MODE); 
    ePaperChangeHdln("Device OFF", true);
    shutdownIAQ(false); 
    return ReturnEvent; 
  }

  switch (currSMState)
  {
    case START_STATE: 
    {
      if(ThisEvent.EventType == ES_INIT)
      {
        sensorsPwrEnable(true); 
        const char* hdlnLabel; 
        uint32_t timerLen; 
        IAQmode_t currIAQMode = STREAM_MODE; 

        esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
        if(wakeup_reason == ESP_SLEEP_WAKEUP_TIMER)
        {
          currSMState = AUTO_STATE; 
          timerLen = AUTO_MODE_TIMER_LEN;  // backup timer 
          currIAQMode = AUTO_MODE; 
          
        } 
        else
        {
          currSMState = STREAM_STATE; 
          timerLen = WARMUP_TIMER_LEN;  // polling timer
          currIAQMode = STREAM_MODE; 
          // if(rtc_get_reset_reason() == POWERON_RESET)
          //   hdlnLabel = "Reading...";  
          // else 
          hdlnLabel = "Warming up..."; 
          ePaperChangeHdln(hdlnLabel, true); 
        }

        ePaperChangeMode(currIAQMode);

        changeSensorsIAQMode(currIAQMode); 
        startSensorsSM(); 
        ES_TimerReturn_t returnVal = ES_Timer_InitTimer(MAIN_SERV_TIMER_NUM, timerLen);  // CO2 sensor has 3 min warmup time
        printf("Going into %s\n", (currIAQMode == STREAM_MODE)? "STREAM MODE" : "AUTO MODE"); 
        if(returnVal == ES_Timer_ERR)
          printf("Main timer err\n");
      }
    }

    case AUTO_STATE:
    {
      if(ThisEvent.EventType == SENSORS_READ_EVENT)
      {
        updateCloudSensorVals(&sensorReads);  // update values to be sent to cloud
        ES_Event_t newEvent = {.EventType=ES_INIT};
        PostCloudService(newEvent);
      }else if(ThisEvent.EventType == CLOUD_UPDATED_EVENT)
      {
        updateScreenSensorVals(&sensorReads, true);
        shutdownIAQ(true); 
      } else if(ThisEvent.EventType == ES_TIMEOUT && ThisEvent.EventParam == MAIN_SERV_TIMER_NUM)
      {
        // 1+ sensor or cloud service didn't respond in time
        printf("Main backup timer timedout\n");
        shutdownIAQ(true); 
      }
      else if(ThisEvent.EventType == ES_SW_BUTTON_PRESS && ThisEvent.EventParam == SHORT_BT_PRESS)
      {
        printf("Changing to stream from auto\n");

        currSMState = STREAM_STATE; 
        changeSensorsIAQMode(STREAM_MODE); 

        if(sensorReads_flag != 0)
        {
          printf("Wait time extended\n");
          ES_Timer_InitTimer(MAIN_SERV_TIMER_NUM, WARMUP_TIMER_LEN);
          startSensorsSM();  // one of the sensors finished, so just restart warmup for simplicity 
        }
        else
        {
          ES_Timer_InitTimer(MAIN_SERV_TIMER_NUM, STREAM_MODE_TIMER_LEN);
        }

        ePaperChangeMode(STREAM_MODE);
        ePaperChangeHdln("Warming up...", true); 
      }
      break;
    }

    case STREAM_STATE: 
    {
      if(ThisEvent.EventType == ES_TIMEOUT && ThisEvent.EventParam == MAIN_SERV_TIMER_NUM)
      {
        ES_Timer_InitTimer(MAIN_SERV_TIMER_NUM, STREAM_MODE_TIMER_LEN);  // screen update timer for stream
        getPMAvg(&(sensorReads.PM10), &(sensorReads.PM25));
        getSVM30Avg(&(sensorReads.eCO2), &(sensorReads.tVOC), &(sensorReads.temp), &(sensorReads.rh)); 
        getCO2Avg(&(sensorReads.CO2));
        //TODO: Don't want to keep retrying wifi if not connected

        if(isTimeSynced())
        {
          updateScreenSensorVals(&sensorReads, false);  
          if(cloudUpdateCounter % CLOUD_COUNTER_LEN == 0)
          {
            updateCloudSensorVals(&sensorReads); 
            ES_Event_t newEvent = {.EventType=ES_INIT};
            PostCloudService(newEvent);
          }
        }
        else
        {
          printf("time not synced\n");
          updateCloudSensorVals(&sensorReads); 
          ES_Event_t newEvent = {.EventType=ES_INIT};
          PostCloudService(newEvent);
          currSMState = STREAM_CLOUD_STATE; 
          ES_Timer_InitTimer(MAIN_SERV_TIMER_NUM, WIFI_TIMEOUT_LEN);
        }
        cloudUpdateCounter++; 
        if(cloudUpdateCounter == CLOUD_COUNTER_LEN)
          cloudUpdateCounter = 0; 
      }
      else if(ThisEvent.EventType == ES_SW_BUTTON_PRESS && ThisEvent.EventParam == SHORT_BT_PRESS)
      {
        printf("Changing to Auto from stream\n");
        currSMState = AUTO_STATE; 
        changeSensorsIAQMode(AUTO_MODE); 
        ePaperChangeMode(AUTO_MODE);
        ePaperChangeHdln("Reading...", true); 
        ES_Timer_InitTimer(MAIN_SERV_TIMER_NUM, AUTO_MODE_TIMER_LEN); 
      }
      
      break;  
    }

    case STREAM_CLOUD_STATE:
    {
      if(ThisEvent.EventType == CLOUD_UPDATED_EVENT )
      {
        updateScreenSensorVals(&sensorReads, false);
        currSMState = STREAM_STATE; 
        ES_Timer_InitTimer(MAIN_SERV_TIMER_NUM, STREAM_MODE_TIMER_LEN);
      }
      else if(ThisEvent.EventType == ES_TIMEOUT && ThisEvent.EventParam == MAIN_SERV_TIMER_NUM)
      {
        printf("no wifi\n");
        ePaperChangeHdln("No Wifi", false); 
        updateScreenSensorVals(&sensorReads, false);
        currSMState = STREAM_STATE; 
        ES_Timer_InitTimer(MAIN_SERV_TIMER_NUM, STREAM_MODE_TIMER_LEN);
      }
    }
  }
  
  return ReturnEvent;
}


void updateCO2Val(int16_t CO2_newVal)
{
  sensorReads.CO2 = CO2_newVal; 
  sensorIdx_t thisSensor = CO2Idx; 
  setFlagBit(thisSensor); 
}

void updateHPMVal(int16_t PM25_newVal, int16_t PM10_newVal)
{
  sensorReads.PM25 = PM25_newVal;
  sensorReads.PM10 = PM10_newVal;  
  sensorIdx_t thisSensor = HPMIdx; 
  setFlagBit(thisSensor); 
}

void updateSVM30Vals(int16_t eCO2_newVal, int16_t tVOC_newVal, int16_t tm_newVal, int16_t rh_newVal)
{
  sensorReads.eCO2 = eCO2_newVal;
  sensorReads.tVOC = tVOC_newVal; 
  sensorReads.temp = tm_newVal; 
  sensorReads.rh = rh_newVal; 
  sensorIdx_t thisSensor = SVM30Idx;
  setFlagBit(thisSensor); 
} 


bool EventCheckerBat()
{
  // return false;  //! BATTERY CHECK IS DISABLED 
  uint16_t batV = getBatVolt(); 
  if(batV < BAT_LOW_THRES)
  {
    ES_Event_t NewEvent = {.EventType=LOW_BAT_EVENT}; 
    PostMainService(NewEvent); 
    return true; 
  }
  return false; 
}




/***************************************************************************
 private functions
 ***************************************************************************/
//shut down the sensors and go into deep sleep
// timedShtdwn as true means timed sleep 
void shutdownIAQ(bool timedShtdwn)
{
  printf("Going into deep sleep\n");
  char str[20]; 
  getCurrTime(str, 20); 
  printf(str); 

  stopHPMMeasurements();  // turns off HPM fan
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  adc_power_off();

  sensorsPwrEnable(false);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_26,1); //1 = High, 0 = Low 
  if(timedShtdwn)
    esp_deep_sleep(900000000UL); // 15 mins
  else 
    esp_deep_sleep_start(); 

}


void setFlagBit(sensorIdx_t sensorBitIdx)
{
  if(sensorBitIdx > 7)
    return; 
  
  sensorReads_flag |= 0x01 << sensorBitIdx; 

  if(sensorReads_flag == ALL_SENSORS_READ)
  {
    ES_Timer_StopTimer(MAIN_SERV_TIMER_NUM);
    sensorReads_flag = 0x00; 
    ES_Event_t newEvent = {.EventType=SENSORS_READ_EVENT};
    RunMainService(newEvent); 
  }
}

void changeSensorsIAQMode(IAQmode_t currIAQMode)
{
  sensorReads_flag = 0; 
  setModeHPM(currIAQMode); 
  setModeSVM30(currIAQMode); 
  setModeCO2(currIAQMode); 
}

void startSensorsSM()
{
  ES_Event_t NewEvent = {.EventType=ES_READ_SENSOR};
  PostHPMService(NewEvent); 
  PostSVM30Service(NewEvent);  // Worst case senario, could hang for 60 secs 
  PostCO2Service(NewEvent); 
}

void initPins()
{
  pinMode(PWR_EN_PIN, OUTPUT); 
  digitalWrite(PWR_EN_PIN, HIGH); 

  pinMode(BUTTON_PIN, INPUT); 
}

void sensorsPwrEnable(bool turnOn)
{
  if(turnOn)
    digitalWrite(PWR_EN_PIN, HIGH);
  else
    digitalWrite(PWR_EN_PIN, LOW);  
}

// shuts down device because battery voltage too low
void shutdownBat()
{
  printf("Low bat. Going to sleep.\n");
  ePaperChangeMode(NO_MODE); 
  ePaperChangeHdln("Device OFF", false);
  ePaperPrintfAlert("Low Battery", "Please plug in and press", "button when charged.");
  delay(1000); 
  shutdownIAQ(false);
}

/*------------------------------- Footnotes -------------------------------*/
/*------------------------------ End of file ------------------------------*/


//**********************************************************************************************************
//*    clock_wifi_radio with weekly schedule and experimental recording function 
//*                                       --  RDA5807 FM Radio which is controlled 
//*                                           by weekly schedule using XIAO ESP32S3. 
//*                                           Clock time of XIAO ESP32S3 refers NTP using wifi network. 
//*                                           This has recording function to SD micro card of XIAO ESP32S3 SENSE.
//**********************************************************************************************************
//  
//
//  2023/2/1 created by asmaro
//  2023/6/10 add server function
//  2023/6/23 add permanent data function
//  2025/10/20 change to apply recording function for XIAO ESP32S3 SENSE
//
#include "Arduino.h"
#include "Audio.h"
#include "SPI.h"
#include "FS.h"
#include "SD.h"
//#include "WiFi.h"
#include <WiFiMulti.h>
#include <WebServer.h>
#include "Wire.h"
#include <Adafruit_GFX.h>       // install using lib tool
#include <Adafruit_SSD1306.h>   // install using lib tool
#include <esp_sntp.h>           // esp lib
#include <TimeLib.h>            // https://github.com/PaulStoffregen/Time
#include <RDA5807.h>            // install using lib tool
#include <Preferences.h>        //For permanent data
#include <driver/i2s.h>
//#include <I2S.h>              // This has a poor function, so not used 

#define VERSION_NO   0.71
#define I2C_SDA      5          // I2C DATA
#define I2C_SCK      6          // I2C CLOCK
#define PIN_SDA  5              // xiao esp32s3 default
#define PIN_SCL  6              // xiao esp32s3 default
#define OLED_I2C_ADDRESS 0x3C   // Check the I2C bus of your OLED device
#define SCREEN_WIDTH 128        // OLED display width, in pixels
#define SCREEN_HEIGHT 64        // OLED display height, in pixels
#define OLED_RESET  -1          // Reset pin # (or -1 if sharing Arduino reset pin)
#define MAXSTNIDX    7          // station index 0-7          
#define MAXSCEDIDX   8          // schedule table index 0-8

#define RECORD_TIME   1         // in seconds, to estimate buffer full time
#define K32  32*1024            // 32KB

//#define SAMPLE_RATE 8000U
//#define SAMPLE_RATE 16000U
#define SAMPLE_RATE 32000U      // most applicable value
//#define SAMPLE_RATE 44100U
//#define SAMPLE_RATE 48000U
#define SAMPLE_BITS 16
#define WAV_HEADER_SIZE 44
#define CHAN_NUM    2           // channel number, stereo is 2 
// I2S to DAC ex. PCM5102
#define I2S_DOUT_A    1
#define I2S_BCLK_A    2
#define I2S_LRC_A     44
#define I2S_NUM_A     1        // DAC I2S port number -> not used currently
#define I2S_DOUT      1  
#define I2S_BCLK      2  
#define I2S_LRC       44
// I2S from DSP Radio
#define I2S_DIN_S       4
#define I2S_BCLK_S      3                                  
#define I2S_LRC_S       43 
// SPI with SD card drive of esp32s3 sense
#define SD_CS         21
#define SPI_MOSI      9
#define SPI_MISO      8
#define SPI_SCK       7

// Wav File recording and reading
#define MAX_RECORD_TIME  30               // Max limited recording time in minutes
#define REC_FREQUENCY  20000000           // 1MHz-24MHz, apply for SPI & SD both
#define I2S_DMA_BUFFER  52                // number of  I2S_DMA_BUFFER

bool stop_read = true;   // priority SD read active in loop()
bool REC_on = false;     // recording on
bool REC_on_no_poff = false;    // recording on but not power off
bool SD_open = false;    // SD open
bool SD_write = false;   // SD write ok
bool WAVE_HDR_write = false; // wavwfile headder wrote
bool I2S_err = false;    // any I2S(DSP) error detected
int last_blk = 0;
int avail_cnt = 0;
uint32_t record_size = (SAMPLE_RATE * SAMPLE_BITS * CHAN_NUM / 8) * RECORD_TIME; // possible size at once in byte
uint8_t *rec_buffer1 = NULL;  // PSRAM
uint8_t *rec_buffer2 = NULL;  // PSRAM
uint8_t *rec_buffer32k = NULL;  // PSRAM
int curr_buf = 1;             // current buff
uint32_t recorded_size = 0;
uint32_t total_recorded_size = 0;
uint32_t estimated_recorded_size = 0;
File file;
char wave_filename[32];
char wave_filename_t[32];
bool wav_2nd_time = false;
int wav_fcount = 1;
bool dsp_active = false;
const int cbl = 50; // circular buffer length
String cb[cbl];     // circular buffer to store SD file name
uint32_t cb_sz[cbl];// circular buffer to store SD file size
uint32_t ct2 = 0;

Audio audio;
WiFiMulti wifiMulti;
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
RDA5807 radio;

String ssid =     "SSID";      // WiFi 1 , set your wifi station
String password = "PASSWORD";  // set your password
String ssid2 =     "SSID2";     // WiFi 2, optional
String password2 = "PASSWORD2"; // set your password

struct tm *tm;
int d_mon ;
int d_mday ;
int d_hour ;
int d_min ;
int d_sec ;
int d_wday ;
int d_year ;
static const char *weekStr[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"}; //3文字の英字

int  vol;    // DSP volume
int  lastvol;
int  stnIdx;
int  laststnIdx;
int  stnFreq[] = {8040, 8250, 8520, 9040, 9150, 7620, 7810, 7860}; // frequency of radio station
String  stnName[] = {"AirG", "NW", "NHK", "STV", "HBC", "sanka", "karos", "nosut"}; // name of radio station max 5 char
//                      0      1     2      3      4       5        6       7
bool bassOnOff = false;
bool vol_ok = true;
bool stn_ok = true;
bool p_onoff_req = false;
bool p_on = false;
int volume,lastVolume;   // inet volume 

float lastfreq;
struct elm {  // program
   int stime; // strat time(min)
   int fidx;  // frequency table index
   int duration; // length min
   int volstep; // volume
   int poweroff; // if 1, power off after duration
   int scheduled; // if 1, schedule done for logic
};
struct elm entity[7][MAXSCEDIDX + 1] = {
{{390,1,59,2,1,0},{540,6,59,1,0,0},{600,0,59,1,0,0},{660,3,119,1,0,0},{780,1,59,1,0,0},{840,0,59,1,0,0},{900,6,59,1,0,0},{1140,3,119,1,0,0},{1410,0,29,1,1,0}}, // sun
{{390,4,59,2,1,0},{480,3,119,1,0,0},{600,6,59,1,0,0},{720,2,119,1,0,0},{840,1,119,1,0,0},{0,0,0,0,0,0},{1020,1,119,1,0,0},{1200,6,89,1,0,0},{1410,0,29,1,1,0}}, // mon
{{390,4,59,2,1,0},{480,3,119,1,0,0},{720,2,89,1,0,0},{840,1,119,1,0,0},{0,0,0,0,0,0},{0,0,0,0,0,0},{1020,1,119,1,0,0},{1200,0,89,1,0,0},{1410,0,29,1,1,0}}, // tue
{{390,4,59,2,1,0},{480,3,119,1,0,0},{720,2,89,1,0,0},{840,1,119,1,0,0},{0,0,0,0,0,0},{0,0,0,0,0,0},{1020,1,119,1,0,0},{1200,0,89,1,0,0},{1410,0,29,1,1,0}}, // wed
{{390,4,59,2,1,0},{480,3,119,1,0,0},{600,6,59,1,0,0},{720,2,119,4,0,0},{840,1,119,1,0,0},{960,1,59,1,0,0},{1080,1,119,1,0,0},{1200,0,89,1,0,0},{1290,3,59,1,1,0}}, // thu
{{390,4,59,2,1,0},{480,3,119,1,0,0},{660,0,59,1,0,0},{720,2,119,1,0,0},{840,6,119,1,0,0},{0,0,0,0,0,0},{1080,1,119,1,0,0},{1200,1,89,1,0,0},{1290,3,59,1,1,1}}, // fri
{{390,0,29,2,0,0},{420,2,119,1,0,0},{540,2,110,1,0,0},{720,2,119,1,0,0},{840,2,119,1,0,0},{960,2,119,1,0,0},{1080,4,59,1,0,0},{1140,3,119,1,0,0},{1260,0,89,1,1,0}}  // sat
};
//struct elm rom_entity[7][MAXSCEDIDX + 1];
int last_d_min = 99;
int currIdx = 99;
int pofftm_h = 0;
int pofftm_m = 0;
const char* ntpServer = "ntp.nict.jp";
const long  gmtOffset_sec = 32400;
const int   daylightOffset_sec = 0;

WebServer server(80);  // port 80(default)

// Operation by server
int s_srv = 1;
int a_srv = 1;
int b_srv = 1;
char titlebuf[166];
char rstr[128];
char stnurl[128];  // current internet station url
String msg = "";
int stoken = 0;      // server token, count up 
int recording = 0;

Preferences preferences; // Permanent data

struct WavHeader_Struct {
  //   RIFF Section
  char RIFFSectionID[4];  // Letters "RIFF"
  uint32_t Size;          // Size of entire file 
  char RiffFormat[4];     // Letters "WAVE"

  //   Format Section
  char FormatSectionID[4];  // letters "fmt"
  uint32_t FormatSize;      // Size of format section
  uint16_t FormatID;        // 1=uncompressed PCM
  uint16_t NumChannels;     // 1=mono,2=stereo
  uint32_t SampleRate;      // 44100, 32000, 16000, 8000 etc.
  uint32_t ByteRate;        // =SampleRate * Channels * (BitsPerSample/8)
  uint16_t BlockAlign;      // =Channels * (BitsPerSample/8)
  uint16_t BitsPerSample;   // 8,16,24 or 32

  // Data Section
  char DataSectionID[4];  // The letters "data"
  uint32_t DataSize;      // Size of the data that follows
} WavHeader;

File WavFile;                                     // SD card directory

void generate_wav_header(uint8_t *wav_header, uint32_t wav_size, uint32_t sample_rate)
{
  // See this for reference: http://soundfile.sapp.org/doc/WaveFormat/
  uint32_t file_size = wav_size + WAV_HEADER_SIZE - 8;
  uint32_t byte_rate = SAMPLE_RATE * SAMPLE_BITS / 8;
  const uint8_t set_wav_header[] = {
    'R', 'I', 'F', 'F', // ChunkID
    file_size, file_size >> 8, file_size >> 16, file_size >> 24, // ChunkSize
    'W', 'A', 'V', 'E', // Format
    'f', 'm', 't', ' ', // Subchunk1ID
    0x10, 0x00, 0x00, 0x00, // Subchunk1Size (16 for PCM)
    0x01, 0x00, // AudioFormat (1 for PCM)
    //0x01, 0x00, // NumChannels (1 channel)
    0x02, 0x00, // NumChannels (2 channel)
    sample_rate, sample_rate >> 8, sample_rate >> 16, sample_rate >> 24, // SampleRate
    byte_rate, byte_rate >> 8, byte_rate >> 16, byte_rate >> 24, // ByteRate
    //0x02, 0x00, // BlockAlign mono
    0x04, 0x00, // BlockAlign stereo
    0x10, 0x00, // BitsPerSample (16 bits)
    'd', 'a', 't', 'a', // Subchunk2ID
    wav_size, wav_size >> 8, wav_size >> 16, wav_size >> 24, // Subchunk2Size
  };
  memcpy(wav_header, set_wav_header, sizeof(set_wav_header));
}

int i2s_install(std::string type) {
  // Set up I2S Processor configuration
  const i2s_config_t i2s_config_dsp = { // for DSP radio
    .mode = i2s_mode_t(I2S_MODE_SLAVE | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = i2s_bits_per_sample_t(16),
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, // R-chan, L-chan
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S), //I2S Philips standard
    .intr_alloc_flags = 0, //
    .dma_buf_count = I2S_DMA_BUFFER,   // #### ex. 52
    .dma_buf_len = 1024,
    .use_apll = false
  };
const i2s_config_t i2s_config_dac = {  // for DAC out
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
  .sample_rate = 32000,  // Note, this will be changed later
  .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
  .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
  .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
  .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,  // high interrupt priority
  .dma_buf_count = 64,                       // 64 buffers
  .dma_buf_len = 64,                         // 64 bytes per buffer
  .use_apll = 0,
  .tx_desc_auto_clear = true,
  .fixed_mclk = -1                           // --> NOT USED
};
 
  int erResult;
  if (type=="DSP") {
    erResult = i2s_driver_install(I2S_NUM_1, &i2s_config_dsp, 0, NULL);
  } else {
    erResult = i2s_driver_install(I2S_NUM_1, &i2s_config_dac, 0, NULL); // if num_0 then panic
  }
  if (erResult!=ESP_OK) Serial.printf("i2s intall %s err(%d)\n", type, erResult);
  return(erResult);
}

int i2s_setpin(std::string type) {
  // Set I2S pin configuration
  const i2s_pin_config_t pin_config_dsp = { // from DSP radio
    .bck_io_num = I2S_BCLK_S,
    .ws_io_num = I2S_LRC_S,
    .data_out_num = -1,
    .data_in_num = I2S_DIN_S
  };
const i2s_pin_config_t pin_config_dac = {  // to DAC out
  .bck_io_num = I2S_BCLK_A,           //  clock 
  .ws_io_num = I2S_LRC_A,             //  Word select
  .data_out_num = I2S_DOUT_A,         //  Data out
  .data_in_num = I2S_PIN_NO_CHANGE    //       --> NOT USED
};
  int erResult;
  if (type=="DSP") {
    erResult = i2s_set_pin(I2S_NUM_1, &pin_config_dsp);
  } else {
    erResult = i2s_set_pin(I2S_NUM_1, &pin_config_dac);
  }
  if (erResult!=ESP_OK) Serial.printf("i2s set pin %s err(%d)\n",type, erResult);
  return(erResult);
}

int split(String data, char delimiter, String *dst){
  int index = 0;
  int arraySize = (sizeof(data))/sizeof((data[0]));
  int datalength = data.length();
  
  for(int i = 0; i < datalength; i++){
    char tmp = data.charAt(i);
    if( tmp == delimiter ){
      index++;
      if( index > (arraySize - 1)) return -1;
    }
    else dst[index] += tmp;
  }
  return (index + 1);
}

int dayofWeek(String dow) {
  dow.trim();
  //Serial.print("dow:");
  //Serial.println(dow);
  if (dow.equals("Sun")) return 0; 
  else if (dow.equals("Mon")) return 1;
  else if (dow.equals("Tue")) return 2;
  else if (dow.equals("Wed")) return 3;
  else if (dow.equals("Thu")) return 4;
  else if (dow.equals("Fri")) return 5;
  else if (dow.equals("Sat")) return 6;
  else return 9;
}

int setWeeksced(String val1){
  String instr[12] = {"\n"};
  String instr2[8] = {"\n"};
  String instr3[4] = {"\n"};
  int ix = split(val1,';',instr);
  if (ix != 11) {
    msg = "different number of arguments.";
    return 4;
  } else {
    //msg = "arguments. ok.";
    int down = dayofWeek(instr[0]);
    if (down > 6) { msg = "invalid day of week."; return 4;}
    else {
      // normal process
      msg = "normal process.";
      instr[0].trim();
      Serial.println(instr[0]);
      for(int j = 0; j <= MAXSCEDIDX; j++) {
        instr[j+1].trim();
        msg = "normal process 2.";
        //Serial.println(instr[j+1]);
        String val2 = instr[j+1];
        ix = split(val2,',',instr2);
        if (ix != 5) { 
            msg = "different number of  2nd level arguments.";
            return 4;
        } else {
            //for(int i = 0; i < 5; i++) {
              msg = "OK! Processing.";
              //Serial.println(instr2[i]);
              val2 = instr2[0];
              ix = split(val2,':',instr3);
              if (ix != 2) {
                msg = "different number of  3rd level arguments.";
                return 4;
              }
              instr3[0].trim();
              instr3[1].trim();
              entity[down][j].stime = instr3[0].toInt() * 60 + instr3[1].toInt();
              instr3[0] = "";
              instr3[1] = "";
              instr2[0] = "";

              entity[down][j].fidx = instr2[1].toInt();
              instr2[1] = "";
              entity[down][j].duration = instr2[2].toInt();
              instr2[2] = "";
              entity[down][j].volstep = instr2[3].toInt();
              instr2[3] = "";
              entity[down][j].poweroff = instr2[4].toInt();
              instr2[4] = "";
              preferences.putString(weekStr[down], val1);  // save permanently              
            //}
        }        
      }
      msg = "OK! Done.";
      return 0;
    }
  } 
}

void handleRoot(void)
{
    String html;
    String val1;
    String val2;
    String val3;
    String val4;
    String val5;
    String val6;
    String html_btn1 = "<p><div class=\"lay_i\"><input type=\"submit\" name=\"record\"  value=\"Recording_Function\" class=\"btn\"></div></p>";
    String html_p1; 
    char htstr[180];
    char stnno[4];
    Serial.println("web received");
    if (server.method() == HTTP_POST) { // submitted with string
      val1 = server.arg("daysced");
      val2 = server.arg("vup");
      val3 = server.arg("vdown");
      val4 = server.arg("stnup");
      val5 = server.arg("stndown");
      val6 = server.arg("pwonoff");
      if (val2.length() != 0) {
        Serial.println("vup");
        vol_setting(); 
        msg = "control: vup";
      }
      else if (val3.length() != 0) {
        Serial.println("vdown");
        vol_setting_2(); 
        msg = "control: vdown";
      }
      else if (val4.length() != 0) {
        Serial.println("stnup");
        station_setting(); 
        msg = "control: stnup";
      }
      else if (val5.length() != 0) {
        Serial.println("stndown");
        station_setting_2(); 
        msg = "control: stndown";
      }
      else if (val6.length() != 0) {
        Serial.println("pwonoff");
        power_onoff_setting(); 
        msg = "control: pwonoff";
      }
      else {
        if (val1.length() == 0) {
          msg ="no input.";
        } else {
          int rc = setWeeksced(val1);
        }
      }
    }
    html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>DSP radio Weekly schedule</title>";
    html += "</head><body><p><h3>DSP Radio Schedule and Recording (ver 0.71)</p></h3>"; // VERSION_NO
    html += "<style>.lay_i input:first-of-type{margin-right: 20px;}</style>";
    html += "<style>.btn {width: 300px; padding: 10px; box-sizing: border-box; border: 1px solid #68779a; background: #cbe8fa; cursor: pointer;}</style>";
    html += "<style>.btn_y {width: 300px; padding: 10px; box-sizing: border-box; border: 1px solid #68779a; background: #ffff8a; cursor: pointer;}</style>";
    html += "<style>.btn_g {width: 300px; padding: 10px; box-sizing: border-box; border: 1px solid #68779a; background: #99ff99; cursor: pointer;}</style>";

    html += "<form action=\"/rec\" method=\"get\">" + html_btn1 + "</form>";
    html += "<form action=\"\" method=\"post\">";
    html += "<input type=\"hidden\" name=\"stoken\" value=\"";
    stoken += 1;
    html += stoken;
    html += "\">"; 
    html += "<p>Select a day of the week, change it, then submit.</p>";
    html += "<p>";
    html += "<input type=\"text\" id=\"daysced\" name=\"daysced\" size=\"120\" value=\"\">";
    html += "</p><p><input type=\"submit\" value=\"submit\" class=\"btn\"></p></form>";
    html += "<p>" + msg + "</p>";
    html += "<p>Arguments of enrty: Start time(hour:min),Station(See below),Duration(min),Volume,Pweroff</p>";
    html += "<p>Station List: 0=" + stnName[0] + ",1=" + stnName[1] + ",2=" + stnName[2] + ",3=" + stnName[3] + ",4=" + stnName[4];
    html += ",5=" + stnName[5] + ",6=" + stnName[6] +  ",7=" + stnName[7] + "</p>";
    html += "<script>";
//    html += "let entity = [[[390,1,59,4,1],[540,6,59,2,0],[600,0,59,2,0],[660,3,119,2,0],[780,1,59,2,0],[840,0,59,2,0],[900,1,59,2,0],[1140,3,119,2,0],[1410,0,29,2,1]],";
//    html += "[[390,1,59,4,1],[540,6,59,2,0],[600,0,59,2,0],[660,3,119,2,0],[780,1,59,2,0],[840,0,59,2,0],[900,1,59,2,0],[1140,3,119,2,0],[1410,0,29,2,1]]]";
    html += "let entity = [";
    for (int i = 0; i < 7; i++){
      html += "[";
      for(int j = 0; j <= MAXSCEDIDX; j++) {
        sprintf(htstr,"['%d:%02d',%d,%d,%d,%d]",entity[i][j].stime / 60,entity[i][j].stime % 60,entity[i][j].fidx,entity[i][j].duration,entity[i][j].volstep,entity[i][j].poweroff);
        html += htstr;
        if (j != MAXSCEDIDX) html += ",";
      }
      html += "]";
      if (i != 6) html += ",";
    }
    html += "];";
    html += "let week = [\"Sun\",\"Mon\",\"Tue\",\"Wed\",\"Thu\",\"Fri\",\"Sat\"];";
    html += "document.write('<table id=\"tbl\" border=\"1\" style=\"border-collapse: collapse\">');";
    html += "for (let i = 0; i < 7; i++){";
    html += "let wstr ='';";
    html += "wstr ='<tr>' + '<td>' + '<input type=\"radio\" name=\"week\" value=\"\" onclick=\"setinput(' + i + ')\">' + '</td>' + '<td>' + week[i] + '</td>';";
    html += "document.write(wstr);";
    html += "for (let j = 0; j < 9; j++){";
    html += "document.write('<td>');";
    html += "document.write(entity[i][j]);";
    html += "document.write('</td>');}";
    html += "document.write('</tr>');";
    html += "}";
    html += "document.write('</table>');";
    html += "function setinput(trnum) {";
    html += "var input = document.getElementById(\"daysced\");";
    html += "var table = document.getElementById(\"tbl\");";
    html += "var cells = table.rows[trnum].cells;";
    html += "let istr = '';";
    html += "for (let j = 1; j <= 10; j++){";
    html += "istr = istr + cells[j].innerText + ';';";
    html += "}";
    html += "input.value = istr;";
    html += "}";
    html += "</script>";
    html += "<style>.lay_i input:first-of-type{margin-right: 20px;}</style>";
    html += "<style>.btn {width: 300px; padding: 10px; box-sizing: border-box; border: 1px solid #68779a; background: #cbe8fa; cursor: pointer;}</style>";
    html += "<p><form action=\"\" method=\"post\">";
    html += "<p>Control Functions</p>";
    html += "<p><div class=\"lay_i\"><input type=\"submit\" name=\"vup\"  value=\"volume up\" class=\"btn\"><input type=\"submit\" name=\"vdown\" value=\"volume down\" class=\"btn\"></div></p>";
    html += "<p><div class=\"lay_i\"><input type=\"submit\" name=\"stnup\"  value=\"station up\" class=\"btn\"><input type=\"submit\" name=\"stndown\" value=\"station down\" class=\"btn\"></div></p>";
    html += "<p><div class=\"lay_i\"><input type=\"submit\" name=\"pwonoff\"  value=\"pwr_on_off\" class=\"btn\"></div></p>";
    html += "</form></p></body>";
    html += "</html>";
    server.send(200, "text/html", html);
    Serial.println("web send response");
}
void handleRec(void)
{
  String html;
  String val1;
  String val2;
  String val3;
  String val4;
  String val5;
  String val6;
  char ts[40];
  //const int cbl = 30; // circular buffer length
  //String cb[cbl];     // circular buffer to store SD file name
  uint32_t total_file_size = 0;
  bool no_refresh = false;

  bool responsed = false;
  String html_btn0 = "<p><div class=\"lay_i\"><input type=\"submit\" name=\"record_start\"  value=\"Start_Recording\" class=\"btn\"><input type=\"submit\" name=\"rec_stop\" value=\"Stop_Recording\" class=\"btn\"></div></p>";
  String html_btn1 = "<p><div class=\"lay_i\"><input type=\"submit\" name=\"record_start\"  value=\"Start_Recording\" class=\"btn_g\"><input type=\"submit\" name=\"rec_stop\" value=\"Stop_Recording\" class=\"btn\"></div></p>";
  String html_btn3 = "<p><div class=\"lay_i\"><input type=\"submit\" name=\"record_start\"  value=\"Recording_in_Progress\" class=\"btn_r\"><input type=\"submit\" name=\"rec_stop\" value=\"Stop_recording\" class=\"btn\"></div></p>";
  String html_btn4 = "<p><div class=\"lay_i\"><input type=\"submit\" name=\"play_stop\"  value=\"Stop_Play\" class=\"btn\"></div></p>";;
  String html_btn5 = "<p><div class=\"lay_i\"><input type=\"submit\" name=\"play_stop\"  value=\"Stop_Play\" class=\"btn_y\"></div></p>";;
  String html_p1, html_p2; 
  html_p1 = html_btn0;
  html_p2 = html_btn4;
  Serial.println("web received(Rec)");
  msg = "";
  if (server.method() == HTTP_POST) { // submitted with string
    val1 = server.arg("record_start");
    val2 = server.arg("rec_stop");
    val3 = server.arg("play_stop");
    val4 = server.arg("stoken");
    val5 = server.arg("format");
    val6 = server.arg("status");
    if (val4.length() != 0) { // server token
      Serial.print("stoken:");
      String s_stoken = server.arg("stoken");
      int t_stoken = s_stoken.toInt();
      Serial.println(s_stoken);
      msg = "stoken:" + s_stoken;
      if (stoken > t_stoken) {
        Serial.println("redirect");
        msg = "Post converted to Get";
        responsed = true;
        server.send(307, "text/html", "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"0;url=/rec\"></head></html>");
        no_refresh = true;
      }
    }
  }
  if (!responsed) {
    if (val2.length() != 0) { // rec stop request   
      Serial.println("rec stop");
      if (REC_on) { // Is recoding active ?
        pofftm_h = (d_hour * 60 + d_min + 1) / 60;
        pofftm_m = (d_hour * 60 + d_min + 1) % 60;
        sprintf(ts,"%02d:%02d %s",pofftm_h,pofftm_m,"poff or recording stop scheduled");
        Serial.println(ts); 
        msg = "control: rec stop scheduled, wait a few minutes";
        REC_on_no_poff = true;
      } else {
        msg = "control: rec stop ignored";        
      }
      no_refresh = true;
    } else 
    if (val3.length() != 0) { // play stop req   
      Serial.println("play_stop");
      msg = "control: play stop";
      stop_read = true;    // Stop read
      no_refresh = true;
    } else
    if (val1.length() != 0) {
        Serial.println("record");
        if (!REC_on && stop_read) {
          total_recorded_size = 0;
          last_blk = 0;
          estimated_recorded_size = MAX_RECORD_TIME * SAMPLE_RATE * SAMPLE_BITS * 60 * 2 / 8;  // MAX_RECORD_TIME min
          pofftm_h = (d_hour * 60 + d_min + MAX_RECORD_TIME) / 60;  // auto stop after MAX_RECORD_TIME min
          pofftm_m = (d_hour * 60 + d_min + MAX_RECORD_TIME) % 60;
          sprintf(ts,"%02d:%02d %s",pofftm_h,pofftm_m,"poff or recording stop scheduled");
          Serial.println(ts); 
          REC_on_no_poff = true;
          esp_err_t err = i2s_start(I2S_NUM_1); // DSP I2S
          if (err != ESP_OK) {
            Serial.println("Failed to initialize I2S!");
            I2S_err = true;
          }
          if(!I2S_err && !SD.begin(SD_CS, SPI, REC_FREQUENCY, "/sd")){ // SD mount
            Serial.println("Failed to mount SD Card!");
            I2S_err = true;
            i2s_stop(I2S_NUM_1);  // DSP I2S
          }
          if (!I2S_err) {
            REC_on = true; // start REC ok
            REC_on_no_poff = true;
            msg = "control: record";
          } else {
            msg = "control: record err";
          }
        } else {
          msg = "control: record ignored";
        }
    } else 
    if (val6.length() != 0) {
      msg = "now recording is acrive";
      no_refresh = true;
    } else
    if (val5.length() != 0){
      msg = "invalid format";
      no_refresh = true;
    } else {
        //nop
    }     
  }
  if (REC_on_no_poff) html_p1 = html_btn3; else html_p1 = html_btn0;
  if (stop_read) html_p2 = html_btn4; else html_p2 = html_btn5;
  if (!responsed) {
    html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>DSP Radio Recording</title>";
    html += "</head><body><p><h3>Recording DSP Radio(experimental)</h3>&nbsp;&nbsp;<a href=\"/\">Back</a></p><form action=\"\" method=\"post\">";
    html += "<style>.lay_i input:first-of-type{margin-right: 20px;}</style>";
    html += "<style>.btn {width: 300px; padding: 10px; box-sizing: border-box; border: 1px solid #68779a; background: #cbe8fa; cursor: pointer;}</style>";
    html += "<style>.btn_y {width: 300px; padding: 10px; box-sizing: border-box; border: 1px solid #68779a; background: #ffff8a; cursor: pointer;}</style>";
    html += "<style>.btn_g {width: 300px; padding: 10px; box-sizing: border-box; border: 1px solid #68779a; background: #99ff99; cursor: pointer;}</style>";
    html += "<style>.btn_r {width: 300px; padding: 10px; box-sizing: border-box; border: 1px solid #68779a; background: #FFA5A5; cursor: pointer;}</style>";
    html += html_p1;
    html += "<input type=\"hidden\" name=\"stoken\" value=\"";
    stoken += 1;
    html += stoken;
    html += "\">"; 
    html += "<p><form action=\"/wavf\" method=\"post\">";
    //html += "<p><div class=\"lay_i\"><input type=\"submit\" name=\"play_stop\"  value=\"STOP PLAY\" class=\"btn\"></div></p>";
    html += html_p2;
    html += "</form></p>";
    html += "<p>Response: " + msg + "</p>";
    html += "<p><h3>Files:</h3>&nbsp;&nbsp;Press 'file name link' to play</p>";
    if(!REC_on) {
      if (!SD.begin(SD_CS, SPI, REC_FREQUENCY, "/sd")){ // SD mount
         Serial.println("Failed to mount SD Card!");
         html += "<p>Failed to mount SD Card!</p>";
      } else {
        File root = SD.open("/");
        uint64_t tb = SD.totalBytes();
        uint32_t tbi = tb/(1024*1024); // MB
        uint64_t ub = SD.usedBytes();
        uint32_t ubi = ub/(1024*1024); // MB
        String sdinfo_tb(tbi);
        String sdinfo_ub(ubi);
        String sdinfo_ra( ( (tbi-ubi) * 1024) / ( ( ( (SAMPLE_RATE * SAMPLE_BITS * CHAN_NUM / 8 ) /1024  ) * 60) )  );
        html += "<p>&nbsp;&nbsp;total size(MB):&nbsp;&nbsp;" + sdinfo_tb; 
        html += "&nbsp;&nbsp;used size(MB):&nbsp;&nbsp;" + sdinfo_ub;
        html += "&nbsp;&nbsp; remaining amount(minutes):&nbsp;&nbsp;" + sdinfo_ra + "</p>";
        bool isDir = false;
        String fname;
        int cbix = 0; // circular buffer index
        int fcnt = 0;
        while (true) {
          String filename = root.getNextFileName(&isDir);
          if (filename == "") break; // nomore files
          if (!isDir) { // not directory
            if (filename.length() > 4) {
              fname = filename.substring(1,28);
              cb[cbix] = fname;
              cbix++;
              fcnt++;
              if (cbix >= cbl) cbix = 0; // reset index
            }
          }
        }
        String sdinfo_fi(fcnt);
        String sdinfo_lf(cbl);
        Serial.printf("filecount: %d\n", fcnt);
        if (fcnt > 0) { //Are there Files?
          html += "<p>&nbsp;&nbsp;total " + sdinfo_fi + " files&nbsp;&nbsp;"; 
          html += "&nbsp;&nbsp;(max listed " + sdinfo_lf + " files)<p>";
          int rdix = 0;
          for (int i = 0; i < cbl && i < fcnt; i++) {
            if (rdix >=  cbl) rdix = 0;
            html += "<p><a href='/wavf?fname=" + cb[rdix] + "'>" + cb[rdix] + "</a>";
            String ts = "/" + cb[rdix];
            char tstr[32] = {'\n'};
            uint32_t fsize;
            File wfile;
            ts.toCharArray(tstr, 29);
            if (!no_refresh) { // aboid to read from SD
              wfile = SD.open((char *)tstr, FILE_READ);
              fsize = wfile.size();
              cb_sz[rdix] = fsize;  // save it
            } else {
              fsize = cb_sz[rdix];  // restore from memory
            }
            total_file_size = total_file_size + fsize/1024;
            int fminutes = fsize / (SAMPLE_RATE * SAMPLE_BITS * CHAN_NUM / 8) / 60 + 1;
            ts = String(fsize/1024);
            html += "&nbsp;&nbsp;&nbsp;&nbsp;file size(KB):&nbsp;&nbsp;" + ts + "&nbsp;&nbsp;";
            ts = String(fminutes);
            html += "&nbsp;&nbsp;length(minutes):&nbsp;&nbsp;" + ts + "</p>";
            if (!no_refresh) wfile.close();
            rdix++;
          }
        } else {
          html += "<p>no files.</p>";
        }   
        root.close();
      }
    } else {
      html += "Recording in progress. If you want to stop recording, press Stop_recording button,<br>";
      html += "and wait a few minutes.<br>";
    }
    html += "</body>";
    html += "</html>";
    Serial.printf("Total file size(KB): %d\n", total_file_size);
    server.send(200, "text/html", html);
  }
  Serial.println("web send response(Rec)");
}

void handleWavf() {
  String html;
  String val1;
  Serial.println("Play start");
  val1 = server.arg(0);
  WiFiClient client = server.client();
  if (!client.connected()) {
    Serial.println("Client disconnected");
    return;
  }
  char tstr[101] = {'/n'};
  val1 = "/" + val1;
  val1.toCharArray(tstr, val1.length() + 1);
  Serial.println(tstr);
  if (!REC_on) {
    WavFile = SD.open((char *)tstr, FILE_READ);  // Open the wav file
    if (WavFile == false)
      Serial.println("Could not open wavfile");
    else {
      if (memcmp(tstr, "/inet_", 6) == 0) { // inet url 
        char inet_url[100] = {'\n'};
        int p = 0;
        while ( WavFile.available() && (p <= 100) )
        {
          char bc[2] = {'\n'};
          WavFile.read((uint8_t*)bc, 1);
          if( (bc[0] == 0x0d) || (bc[0] == 0x0a) || (bc[0] == 0x20) ) break;
          inet_url[p] = bc[0];
          p++;
        }
        if ( (p > 10) && (p <= 99) ) {  // may be url
          Serial.printf("inet detected: %s\n", inet_url);
          bool conn_ok = audio.connecttohost(inet_url);
          if (conn_ok) {
            stop_read = false; // ok, start it
          } else {
            stop_read = true; // cannot connect
          }
        }
        WavFile.close();
      }   else   {
        WavFile.read((byte*)&WavHeader, 44);                    // Read  WAV header, first 44 bytes of the file.
        int rc = DumpWAVHeader(&WavHeader);                     // confirm  header data
        WavFile.close();  //
        if (rc <= 1) {  // wav or mp3 ?
          i2s_start(I2S_NUM_0); // start port of I2S Audio
          delay(100);
          bool cc = audio.connecttoFS(SD, tstr); // play this file in the SD
          if (cc) {
            stop_read = false; // ok, start it  
          } else {
            Serial.printf("connectFS fail");
            stop_read = true; // stop it  
          }
        }
      }
    }
  } else {
    msg = "Now recording is active";
    server.send(302, "text/html", "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"0;url=/rec?status=invalid\"></head></html>");
  }
  if (!stop_read) {
    //server.send(200, "text/plain", "Ok Play start. To stop Play, press <a href=\"/rec\">backward</a>, then press Stop_Play button on the screen.");
    server.send(200, "text/html", "<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body>Ok Play start. To stop Play, press &nbsp;<a href=\"/rec\">backward</a>, then press Stop_Play button on the screen.</body></html>");
    Serial.println("Play continue");
  } else {
    msg = "invalid format";
    // never use 301 redirect, it's parmanent. 
    server.send(302, "text/html", "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"0;url=/rec?format=invalid\"></head></html>");
  }
}
void handleNotFound(void)
{
  server.send(404, "text/plain", "Not Found.");
}

void SDCardInit() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);  // SD card chips select, must use GPIO 21 (ESP323 sense)
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  SPI.setFrequency(REC_FREQUENCY);  // 10 - 24MHz
  if (!SD.begin(SD_CS, SPI, REC_FREQUENCY, "/sd")) {
    Serial.println("Error talking to SD card!");
  }
}
void setup()
{
  Serial.begin(115200);
  Serial.println("start");
  //SDCardInit();

  //pinMode(1, INPUT_PULLUP);  // mode_setting
  //pinMode(3, INPUT_PULLUP);  // station_setting
  //pinMode(4, INPUT_PULLUP);  // power on_off
  //digitalWrite(1, HIGH);
  //digitalWrite(3, HIGH);
  //digitalWrite(4, HIGH);
  //attachInterrupt(1, vol_setting, FALLING); 
  //attachInterrupt(3, station_setting, FALLING); 
  //attachInterrupt(4, power_onoff_setting, FALLING); 

  Wire.setPins(PIN_SDA, PIN_SCL);  
  Wire.begin(); //
  Wire.setClock(400000);
  dsp_active = true;

  Wire.beginTransmission(0x11);
  Wire.write(0x04); // REG4
  Wire.write(0b10001000); // RDSIEN, De-emphasis 50μs
  Wire.write(0b01000000); // I2S Enabled
  Wire.endTransmission(); // stop transmitting
  Wire.beginTransmission(0x011);
  Wire.write(0x06); // REG6
  Wire.write(0b00000010); //  MASTER, DATA_SIGNED
  //Wire.write(0b00000000); //  MASTER, DATA_UNSIGNED #####2025/10/3
  //Wire.write(0b00010010); // SLAVE, DATA_SIGNED #####2025/10/3
  //Wire.write(0b10000000); // 48KBPS
  //Wire.write(0b01110000); // 44.1KBPS
  Wire.write(0b01100000); // 32KBPS
  //Wire.write(0b00000000); // 8KBPS
  //Wire.write(0b00110000); // 16KBPS
  Wire.endTransmission(); // stop transmitting

  oled.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS);
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  // Permanent data check
  preferences.begin("week_sced", false);
  for (int i = 0; i < 7; i++){
    String val1 = preferences.getString(weekStr[i],"");       
    if (val1 != "") {
      //Serial.println(val1);
      int rc = setWeeksced(val1);
    }
  }
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(ssid.c_str(), password.c_str());  
  wifiMulti.addAP(ssid2.c_str(), password2.c_str());
  wifiMulti.run();   // It may be connected to strong one
  
  while (true) {
    if(WiFi.status() == WL_CONNECTED){ break; }  // WiFi connect OK then next step
    Serial.println("WiFi Err");
    oled.setTextSize(2); // Draw 2X-scale text
    oled.setCursor(0, 0);
    oled.print("WiFi Err");
    oled.display();
    WiFi.disconnect(true);
    delay(5000);
    wifiMulti.run();
    delay(1000*300);  // Wait for Wifi ready
  }
  Serial.println("wifi start");
  wifisyncjst(); // refer time and day
  splash();
  radio.setup(); // Stats the receiver with default valuses. Normal operation
  delay(500);
  radio.setBand(2); //
  radio.setSpace(0); //
  delay(300);
  p_on = true;
  vol = preferences.getInt("vol_r", -1);
  if (vol < 0)  vol = 1;
  radio.setVolume(vol);
  lastvol=vol;
  stnIdx = preferences.getInt("stix", -1);
  if (stnIdx < 0)  stnIdx = 3;
  lastfreq = stnFreq[stnIdx];
  laststnIdx = stnIdx;
  radio.setFrequency(lastfreq);  // Tune on last
  // web server
  server.on("/", handleRoot);
  server.on("/wavf", handleWavf);
  server.on("/rec", handleRec);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.print("IP = ");
  Serial.println(WiFi.localIP());
  titlebuf[0] = 0;

  // PSRAM malloc for recording
  rec_buffer1 = (uint8_t *)ps_malloc(record_size);
  rec_buffer2 = (uint8_t *)ps_malloc(record_size);
  rec_buffer32k = (uint8_t *)ps_malloc(1024*32);
  if (rec_buffer1 != NULL && rec_buffer2 != NULL && rec_buffer32k != NULL) {
    memset(rec_buffer32k, 0, 1024*32); // 0 clear
  } else { 
    Serial.printf("malloc failed!\n");
    I2S_err = true;    
  }
  Serial.printf("Buffer: %d bytes\n", ESP.getPsramSize() - ESP.getFreePsram());
  // I2S set up
  SDCardInit();
  // DSP OUT
  i2s_install("DSP");
  i2s_setpin("DSP");
  i2s_stop(I2S_NUM_1);  // use I2S_NUM_1 port
  // DAC --> not used
  //i2s_install("DAC");
  //i2s_setpin("DAC");
  //i2s_stop(I2S_NUM_1);
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);  // use I2S_NUM_0 port
  volume = preferences.getInt("vol", -1);
  if (volume < 0) volume = 3;
  audio.setVolume(volume); // 0...21
  lastVolume = volume;
  i2s_stop(I2S_NUM_0); // stop I2S of audio
  wav_fcount = preferences.getInt("wavf_no", -1);
  if (wav_fcount < 0) wav_fcount = 1;
  Serial.println("Setup done");

}
void splash()
{
  IPAddress ipadr = WiFi.localIP();
  oled.setTextSize(2); // Draw 2X-scale text
  oled.setCursor(0, 0);
  oled.print("Clock");
  oled.setCursor(0, 15);
  oled.print("Radio_Rec");
  oled.display();
  delay(500);
  oled.setCursor(0, 30);
  oled.printf("IP:%d.%d", ipadr[2],ipadr[3]); // display last octet
  oled.setCursor(0, 45);
  oled.printf("V%.2f", VERSION_NO);
  oled.display();
  delay(1000);
}

void wifisyncjst() {
  //---------内蔵時計のJST同期--------
  // NTPサーバからJST取得
  int lcnt = 0;
  //configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  configTzTime("JST-9", "ntp.nict.jp", "ntp.jst.mfeed.ad.jp");
  delay(500);
  // 内蔵時計の時刻がNTP時刻に合うまで待機
  while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) {
    delay(500);
    lcnt++;
    if (lcnt > 100) {
      Serial.println("time not sync within 50 sec");
      break;
    }
  }
}

void loop()
{
  //---------内蔵時計の表示--------
  char ts[80];
  float tf;
  if (stop_read) {  // stop_read is false on ordinary case except SD read mode
    time_t t = time(NULL);
    tm = localtime(&t);
    d_mon  = tm->tm_mon+1;
    d_mday = tm->tm_mday;
    d_hour = tm->tm_hour;
    d_min  = tm->tm_min;
    d_sec  = tm->tm_sec;
    d_wday = tm->tm_wday;
    d_year = tm->tm_year;
    //Serial.print("time ");
    sprintf(wave_filename_t, "/mug%04d%02d%02d%02d%02d%02d_", d_year + 1900, d_mon, d_mday, d_hour, d_min, d_sec);
    sprintf(ts, "%02d-%02d %s", d_mon, d_mday, weekStr[d_wday]);
    oled.setTextSize(2); // Draw 2X-scale text
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.print(ts);
    //Serial.println(ts);
    sprintf(ts,"%02d:%02d:%02d",d_hour,d_min,d_sec);
    oled.setCursor(0, 15);
    oled.print(ts);
    //Serial.println(ts);
    int pi = p_on ? 1 : 0;
    sprintf(ts, "%s%02d %s%01d", "Vol:", vol, "P:", pi);
    oled.setCursor(0, 30);
    oled.print(ts);
    tf = lastfreq/100.0;
    sprintf(ts, "%3.1f S:%03d", tf, radio.getRssi()); // frequency and signal strength
    oled.setCursor(0, 45);
    oled.print(ts);
    oled.display();
    // check web server req
    server.handleClient();
    if (p_onoff_req) {
      if (p_on) {
        radio.powerDown();
        Serial.println("pw off");
        p_on = false;
      } else {
        radio.powerUp();
        delay(300);
        radio.setVolume(vol);
        radio.setFrequency(stnFreq[stnIdx]);
        delay(300);
        p_on = true;
      }
      p_onoff_req = false;
    }
    if (lastvol != vol || lastVolume != volume) {
      Serial.println("vol changed");
      if (stop_read) {
        radio.setVolume(vol);
        preferences.putInt("vol_r",vol);
      } else {
        audio.setVolume(volume);
        preferences.putInt("vol",volume);
      }
      vol_ok = true;
      lastvol = vol;
      lastVolume = volume;
    }
    if (laststnIdx != stnIdx) {
      Serial.println("stn changed");
      radio.setFrequency(stnFreq[stnIdx]);
      lastfreq = stnFreq[stnIdx];
      laststnIdx = stnIdx;
      preferences.putInt("stix", stnIdx); // save
      stn_ok = true;
    }
 
    if (last_d_min != d_min) {
      last_d_min = d_min;
      if (pofftm_h == d_hour && pofftm_m == d_min && p_on) { // power off time ?
        p_onoff_req = true;
        pofftm_h = 0;
        pofftm_m = 0;
        if (REC_on) {
          // note : abandon remainning record in the buffer, which is not so important.
          uint8_t wav_header[WAV_HEADER_SIZE];
          file.seek(0);
          generate_wav_header(wav_header, total_recorded_size, SAMPLE_RATE);
          file.write(wav_header, WAV_HEADER_SIZE);
          Serial.printf("WAVE file header updated.\n");
          file.close();
          Serial.printf("Last recorded %d, Total %d bytes.\n", recorded_size, total_recorded_size); 
          Serial.printf("The recording is over.\n");
          recorded_size = 0;
          REC_on = false;
          WAVE_HDR_write = false;
          I2S_err = false;
          if (REC_on_no_poff) {
            p_onoff_req = false;
            REC_on_no_poff = false;
          }
        }
      } else 
      {
        for(int i = 0; i <= MAXSCEDIDX; i++) {
          if (entity[d_wday][i].stime == 0 ) {     
            //nop
            //Serial.println(d_min);
          } else 
          {
            //Serial.println(entity[d_wday][i].stime);
            if ((entity[d_wday][i].stime <= d_hour * 60 + d_min) && 
                ((entity[d_wday][i].stime + entity[d_wday][i].duration) >= (d_hour * 60 + d_min ))
                && (entity[d_wday][i].scheduled != 1)) {
              if (lastfreq == stnFreq[entity[d_wday][i].fidx]) {
                //entity[d_wday][i].scheduled = 1; // mark it scheduled
              
              } else {          
                //radio.setFrequency(stnFreq[entity[d_wday][i].fidx]);  #########
                stnIdx =  entity[d_wday][i].fidx;              
                //lastfreq = stnFreq[stnIdx];
              }
              //radio.setVolume(entity[d_wday][i].volstep);
              vol = entity[d_wday][i].volstep;
              currIdx = i;
              entity[d_wday][i].scheduled = 1; // mark it scheduled
              Serial.println("scheduled");
              if (entity[d_wday][i].poweroff==1 || entity[d_wday][i].poweroff==4 || entity[d_wday][i].poweroff==5) { // power off or REC?
                pofftm_h = (entity[d_wday][i].stime + entity[d_wday][i].duration) / 60; // set power off time
                pofftm_m = (entity[d_wday][i].stime + entity[d_wday][i].duration) % 60;
                sprintf(ts,"%02d:%02d %s",pofftm_h,pofftm_m,"poff or recording stop scheduled.");
                Serial.println(ts);
                if (entity[d_wday][i].poweroff==4 || entity[d_wday][i].poweroff==5) 
                  { // REC start
                    //I2S.setAllPins(2, 44, 4, -1, -1);
                    //I2S.setAllPins(sclkPin, wsPin, sdataPin, outSdPin, inSdPin);
                    total_recorded_size = 0;
                    last_blk = 0;
                    estimated_recorded_size = entity[d_wday][i].duration * SAMPLE_RATE * SAMPLE_BITS * 60 * 2 / 8;  // 
                    //if (!I2S_err && !I2S.begin(I2S_PHILIPS_MODE, SAMPLE_BITS)) {  // slave mode
                    //if (!I2S_err && !I2S.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {  // slave mode
                    //if (!I2S_err && !I2S.begin(I2S_PHILIPS_MODE, SAMPLE_BITS)) {  // slave mode
                    //if (!I2S_err && !(i2s_start(I2S_NUM_0))) {  // slave mode
                    esp_err_t err = i2s_start(I2S_NUM_1);
                    //esp_err_t err = i2s_install("DSP");
                    //err = i2s_setpin("DSP");
                    if (err != ESP_OK) {
                      Serial.println("Failed to initialize I2S!");
                      I2S_err = true;
                      //while (1) ;
                    }
                    if(!I2S_err && !SD.begin(SD_CS, SPI, REC_FREQUENCY, "/sd")){ // SD mount
                      Serial.println("Failed to mount SD Card!");
                      I2S_err = true;
                      i2s_stop(I2S_NUM_1);
                      //i2s_driver_uninstall(I2S_NUM_0);
                      //while (1) ;
                    }
                    if (!I2S_err) {
                      REC_on = true; // start REC ok
                      if (entity[d_wday][i].poweroff==4) REC_on_no_poff = true;
                    }
                  }
                }
                if (p_on==false) {
                  p_onoff_req = true;  //  if power off currently then power on req
                  pofftm_h = 0;        // reset
                  pofftm_m = 0;
                  Serial.println("pw on req");
                }  
              }
          }
        }
      }
    }
    if (REC_on && !I2S_err) {
      // Start recording
      uint32_t sample_size = 0;
      uint8_t *rec_buffer = NULL;
      uint32_t avail_size = 0;
      uint32_t BytesWritten = 0;
      //avail_size = I2S.available();
      //avail_size = 1024 * I2S_DMA_BUFFER;  // Be equal to DMA buff size
      avail_size = 1024 * 32;  // 
      if (curr_buf==1) rec_buffer = rec_buffer1; else rec_buffer = rec_buffer2;
      //esp_i2s::i2s_read(esp_i2s::I2S_NUM_0, rec_buffer + recorded_size, avail_size, &sample_size, portMAX_DELAY);
      i2s_read(I2S_NUM_1, rec_buffer + recorded_size, avail_size, &sample_size, 1); //
      if (sample_size == 0) {
        //Serial.printf("Record Failed!\n");
        //I2S_err = true;
        ;
      } else { // read ok
        recorded_size =  recorded_size + sample_size;
        avail_cnt ++;
        if (recorded_size >= K32*2) {
          if (curr_buf==1) {
            // switch buffer area
            memcpy(rec_buffer2, rec_buffer1 + (K32*2), recorded_size - (K32*2));
            rec_buffer = rec_buffer2 + recorded_size - (K32*2);
            curr_buf = 2;
          } else { // curr_buff 2
            memcpy(rec_buffer1, rec_buffer2 + (K32*2), recorded_size - (K32*2));
            rec_buffer = rec_buffer1 + recorded_size - (K32*2);
            curr_buf = 1;
          }
          recorded_size = recorded_size - (K32*2);
          SD_write = true; 
        }
      }
      if (SD_write) {
        // write SD
        if (!WAVE_HDR_write) {
          // write wave file header
          sprintf(wave_filename, "%s%d.wav", wave_filename_t, wav_fcount);
          wav_fcount++;
          preferences.putInt("wavf_no", wav_fcount);
          file = SD.open(wave_filename, FILE_WRITE);

          // Write the header to the WAV file
          uint8_t wav_header[WAV_HEADER_SIZE];
          generate_wav_header(wav_header, estimated_recorded_size, SAMPLE_RATE);
          memset(rec_buffer32k, 0, 1024*32);
          //file.write(wav_header, WAV_HEADER_SIZE);
          memcpy(rec_buffer32k, wav_header, WAV_HEADER_SIZE);
          file.write(rec_buffer32k, 1024 * 32); // filler
          total_recorded_size = K32;
          Serial.printf("WAVE file header wrote.\n");
          WAVE_HDR_write = true;
        }
        // write SD data
        if (total_recorded_size/(K32*10) != last_blk) {
           Serial.printf("Available %d times,Left over %d bytes, use buff %d.\n", avail_cnt, recorded_size, curr_buf);
           last_blk = total_recorded_size / (K32*10);
        }
        //Serial.printf("Writing to the file ...\n");
        if (curr_buf==1) rec_buffer = rec_buffer2; else rec_buffer = rec_buffer1;
        int w_sz = file.write(rec_buffer, K32/*recorded_size*/); 
        w_sz = file.write(rec_buffer + K32, K32/*recorded_size*/); 

        //if (file.write(rec_buffer, recorded_size) != recorded_size) {
        if (w_sz != K32/*recorded_size*/) {
          // Retry it, once
          delay(10);
          int w_sz_r = file.write(rec_buffer + w_sz, K32/*recorded_size*/ - w_sz); 
          if (w_sz_r != K32/*recorded_size*/ - w_sz) {
            Serial.printf("Write file and retry Failed! wz:%d, rd:%d\n", w_sz + w_sz_r, recorded_size);
            I2S_err = true;
          } else {
            Serial.printf("Write file failed, and retry success! wz:%d, rd:%d\n", w_sz + w_sz_r, recorded_size);
          }
          total_recorded_size = total_recorded_size + w_sz + w_sz_r;
        } else  total_recorded_size = total_recorded_size + K32*2/*recorded_size*/;
        //if (curr_buf==1) curr_buf = 2; else curr_buf = 1; // switch buffer area
        //recorded_size = 0;
        avail_cnt = 0;       
        SD_write = false;
      }

    }

  } else {  // SD read mode
    if (dsp_active){
      radio.powerDown();
      p_on = false;
      dsp_active = false;
    } 
    if (lastVolume != volume) {
      audio.setVolume(volume);
      preferences.putInt("vol",volume);
      vol_ok = true;
      lastVolume = volume;
    }
    server.handleClient();
    audio.loop();
    if (stop_read) {
      p_onoff_req = true;
      dsp_active = true;
      audio.stopSong();
      i2s_stop(I2S_NUM_0);
      Serial.println("Play end.");
    }
  }
}
void vol_setting() {
  if (vol_ok) {  // wait last req
     vol_ok = false;
    if (stop_read) { // DSP
      vol++;
      if (vol > 8) vol = 1; // turn around to support single button
    } else { // audio
      volume++;
      if (volume > 12) vol = 1; // turn around to support single button
    }
  }
}
void vol_setting_2() { 
  if (vol_ok) {  // wait last req
    vol_ok = false;
    if (stop_read) { // DSP
      vol--;
      if (vol < 0) {
        vol = 0;
        lastvol = 1;
      } 
    } else { // Audio
      volume--;
      if (volume < 0) {
        volume = 0;
        lastVolume = 1;
      }      
    }
  }
}
void station_setting_2() {
  if (stn_ok) {  // wait last req
    stnIdx--;
    stn_ok = false;
    if (stnIdx < 0) stnIdx = MAXSTNIDX; // turn around to support single button
  }
}
void station_setting() {
  if (stn_ok) {  // wait last req
    stnIdx++;
    stn_ok = false;
    if (stnIdx > MAXSTNIDX) stnIdx = 0;  // turn around to support single button
  }
}
void power_onoff_setting() {
  if (p_onoff_req==false) {  // wait last req
     p_onoff_req = true;  // req
  }
}

int DumpWAVHeader(WavHeader_Struct* Wav) {
  if (memcmp(Wav->RIFFSectionID, "RIFF", 4) != 0) {
    Serial.print("Not a RIFF format file - ");
    PrintData(Wav->RIFFSectionID, 4);
    if (memcmp(Wav->RIFFSectionID, "ID3", 3) == 0) {
      Serial.println(" May be a MP3 format file.");
      return (1);
    }
    return(5);
  } 
  if (memcmp(Wav->RiffFormat, "WAVE", 4) != 0) {
    Serial.print("Not a WAVE file - ");
    PrintData(Wav->RiffFormat, 4);
    return(4);
  }
  if (memcmp(Wav->FormatSectionID, "fmt", 3) != 0) {
    Serial.print("fmt ID not present - ");
    PrintData(Wav->FormatSectionID, 3);
    return(3);
  }
  if (memcmp(Wav->DataSectionID, "data", 4) != 0) {
    Serial.print("data ID not present - ");
    PrintData(Wav->DataSectionID, 4);
    return(2);
  }
  // All looks good, dump the data
  Serial.print("Total size :");
  Serial.println(Wav->Size);
  Serial.print("Format section size :");
  Serial.println(Wav->FormatSize);
  Serial.print("Wave format :");
  Serial.println(Wav->FormatID);
  Serial.print("Channels :");
  Serial.println(Wav->NumChannels);
  Serial.print("Sample Rate :");
  Serial.println(Wav->SampleRate);
  Serial.print("Byte Rate :");
  Serial.println(Wav->ByteRate);
  Serial.print("Block Align :");
  Serial.println(Wav->BlockAlign);
  Serial.print("Bits Per Sample :");
  Serial.println(Wav->BitsPerSample);
  Serial.print("Data Size :");
  Serial.println(Wav->DataSize);
  return(0);
}

void PrintData(const char* Data, uint8_t NumBytes) {
  for (uint8_t i = 0; i < NumBytes; i++)
    Serial.print(Data[i]);
  Serial.println();
}
// optional
void audio_info(const char *info){
    Serial.print("info        "); Serial.println(info);
}
void audio_id3data(const char *info){  //id3 metadata
    Serial.print("id3data     ");Serial.println(info);
}
void audio_eof_mp3(const char *info){  //end of file
    stop_read = true; // END OF play file
    Serial.print("eof_mp3     ");Serial.println(info);
}
void audio_showstation(const char *info){
    Serial.print("station     ");Serial.println(info);
}
void audio_showstreamtitle(const char *info){
    Serial.print("streamtitle ");Serial.println(info);
}
void audio_bitrate(const char *info){
    Serial.print("bitrate     ");Serial.println(info);
}
void audio_commercial(const char *info){  //duration in sec
    Serial.print("commercial  ");Serial.println(info);
}
void audio_icyurl(const char *info){  //homepage
    if (strlen(info) == 0) stop_read = true; // maybe connect error 
    Serial.print("icyurl      ");Serial.println(info);
}
void audio_lasthost(const char *info){  //stream URL played
    Serial.print("lasthost    ");Serial.println(info);
}

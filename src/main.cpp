#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>

// WAVESHARE ESP32-S3 TOUCH LCD 1.85, USB-C DOWN
Arduino_DataBus *bus = new Arduino_ESP32QSPI(21, 40, 46, 45, 42, 41);
Arduino_GFX *gfx = new Arduino_ST77916(bus, -1, 0, true, 360, 360);
LV_IMG_DECLARE(porsche_bg);
static constexpr int W=360, H=360, CX=180, CY=180, TRAIL=14;
static constexpr uint8_t IMU_ADDR=0x6B;
static constexpr uint8_t SCREEN_ROTATION=0;
// If LEFT/RIGHT or BRAKE/ACCEL is reversed, change the respective sign.
static constexpr float LAT_SIGN=-1.0f, LONG_SIGN=1.0f;
static lv_color_t drawBuf[W*24];
static lv_disp_draw_buf_t lvDraw;
static lv_disp_drv_t lvDriver;
static lv_obj_t *dots[TRAIL], *peakLatDot, *peakLongDot;
static lv_obj_t *peakLatText, *peakLongText, *statusText;
static uint32_t resetNoticeUntil=0;
static constexpr uint8_t TOUCH_ADDR=0x15; // CST816-family I2C touch (verify on hardware)
static bool touchWasDown=false;
static uint32_t lastTapMs=0;
static float peakLat=0.0f, peakLong=0.0f;
static float histX[TRAIL]={},histY[TRAIL]={};
static float zeroX=0,zeroY=0;
static bool imuOK=false;

static lv_color_t C(uint32_t rgb){return lv_color_hex(rgb);}
static void flush_cb(lv_disp_drv_t *drv,const lv_area_t *a,lv_color_t *pixels){
  int w=a->x2-a->x1+1,h=a->y2-a->y1+1;
  gfx->draw16bitRGBBitmap(a->x1,a->y1,(uint16_t*)pixels,w,h);
  lv_disp_flush_ready(drv);
}
static bool writeReg(uint8_t reg,uint8_t val){
  Wire.beginTransmission(IMU_ADDR);Wire.write(reg);Wire.write(val);
  return Wire.endTransmission()==0;
}
static bool readRegs(uint8_t reg,uint8_t *out,size_t n){
  Wire.beginTransmission(IMU_ADDR);Wire.write(reg);
  if(Wire.endTransmission(false)!=0)return false;
  if(Wire.requestFrom(IMU_ADDR,n)!=n)return false;
  for(size_t i=0;i<n;i++)out[i]=Wire.read();
  return true;
}
static bool initIMU(){
  uint8_t id=0;
  if(!readRegs(0x00,&id,1)||id!=0x05)return false;
  return writeReg(0x02,0x60)&&writeReg(0x03,0x23)&&writeReg(0x08,0x01);
}
static bool readAccel(float &x,float &y,float &z){
  uint8_t b[6];if(!readRegs(0x35,b,6))return false;
  int16_t ax=(int16_t)((b[1]<<8)|b[0]);
  int16_t ay=(int16_t)((b[3]<<8)|b[2]);
  int16_t az=(int16_t)((b[5]<<8)|b[4]);
  x=ax/4096.0f;y=ay/4096.0f;z=az/4096.0f;
  return true;
}
static lv_obj_t *label(lv_obj_t *parent,const char *txt,const lv_font_t *font,uint32_t color){
  lv_obj_t *o=lv_label_create(parent);
  lv_label_set_text(o,txt);
  lv_obj_set_style_text_font(o,font,0);
  lv_obj_set_style_text_color(o,C(color),0);
  return o;
}
static void makeUI(){
  lv_obj_t *root=lv_scr_act();
  lv_obj_clear_flag(root,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(root,C(0x080A0C),0);
  lv_obj_set_style_bg_opa(root,LV_OPA_COVER,0);
  // Background first: all other objects are drawn above it.
  lv_obj_t *bg=lv_img_create(root);
  lv_img_set_src(bg,&porsche_bg);
  lv_obj_center(bg);
  lv_obj_clear_flag(bg,LV_OBJ_FLAG_CLICKABLE);

  // Live G-force dot trail, in front of the Porsche artwork.
  for(int i=TRAIL-1;i>=0;i--){
    dots[i]=lv_obj_create(root);
int d=i==0?18:5;
    lv_obj_set_size(dots[i],d,d);
    lv_obj_set_style_radius(dots[i],LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_border_width(dots[i],0,0);
    lv_obj_set_style_pad_all(dots[i],0,0);
    lv_obj_set_style_bg_color(dots[i],C(i==0?0xFFF7E6:0xE59A3B),0);
    lv_obj_set_style_bg_opa(dots[i],i==0?LV_OPA_COVER:LV_OPA_60,0);
    lv_obj_set_style_shadow_width(dots[i],i==0?12:0,0);
    lv_obj_set_style_shadow_color(dots[i],C(0xE59A3B),0);
    lv_obj_set_pos(dots[i],CX-d/2,CY-d/2);
    lv_obj_clear_flag(dots[i],LV_OBJ_FLAG_SCROLLABLE);
  }
  // Two small persistent peak dots and their compact values.
  // These replace the overlapping LIVE/MAX text rows.
  peakLatDot=lv_obj_create(root);
  peakLongDot=lv_obj_create(root);
  lv_obj_t *markers[]={peakLatDot,peakLongDot};
  for(auto *o:markers){
    lv_obj_set_size(o,9,9);
    lv_obj_set_style_radius(o,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_bg_color(o,C(0xFFB34E),0);
    lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_border_width(o,0,0);
    lv_obj_set_style_pad_all(o,0,0);
    lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o,CX-4,CY-4);
  }
  peakLatText=label(root,"",&lv_font_montserrat_12,0xFFCB82);
  peakLongText=label(root,"",&lv_font_montserrat_12,0xFFCB82);
  statusText=label(root,"",&lv_font_montserrat_12,0xFFCB82);
  lv_obj_align(statusText,LV_ALIGN_TOP_MID,0,76);

}
static void updateUI(float gx,float gy){
  // Peak hold: update only when absolute G-force exceeds the prior peak.
  // Ignore tiny stationary noise; the peaks remain until reboot.
  if(fabsf(gx)>fabsf(peakLat) && fabsf(gx)>=0.05f) peakLat=gx;
  if(fabsf(gy)>fabsf(peakLong) && fabsf(gy)>=0.05f) peakLong=gy;
  for(int i=TRAIL-1;i>0;i--){histX[i]=histX[i-1];histY[i]=histY[i-1];}
  histX[0]=gx;histY[0]=gy;
  for(int i=TRAIL-1;i>=0;i--){
    int d=i==0?12:5;
    int x=CX+(int)lroundf(constrain(histX[i],-1.3f,1.3f)*95)-d/2;
    int y=CY+(int)lroundf(constrain(histY[i],-1.3f,1.3f)*95)-d/2;
    lv_obj_set_pos(dots[i],x,y);
  }
  // Mark peak lateral G on horizontal axis, longitudinal on vertical.
  // The small numbers follow their marker and stay within the round gauge.
  const int radius=106;
  int px=CX+(int)lroundf(constrain(peakLat,-1.3f,1.3f)*radius/1.3f);
  int py=CY+(int)lroundf(constrain(peakLong,-1.3f,1.3f)*radius/1.3f);
  lv_obj_set_pos(peakLatDot,px-4,CY-4);
  lv_obj_set_pos(peakLongDot,CX-4,py-4);
  char txt[24];
  snprintf(txt,sizeof(txt),"%.2fg",fabsf(peakLat));
  lv_label_set_text(peakLatText,txt);
  lv_obj_update_layout(peakLatText);
  int lw=lv_obj_get_width(peakLatText);
  int lx=(peakLat>=0)?px-lw-10:px+10;
  lv_obj_set_pos(peakLatText,constrain(lx,68,290-lw),CY-25);
  snprintf(txt,sizeof(txt),"%.2fg",fabsf(peakLong));
  lv_label_set_text(peakLongText,txt);
  lv_obj_update_layout(peakLongText);
  int ww=lv_obj_get_width(peakLongText);
  lv_obj_set_pos(peakLongText,CX+12,constrain(py+(peakLong>=0?-23:9),70,278));
  lv_label_set_text(statusText,!imuOK?"IMU NOT FOUND":
                    (millis()<resetNoticeUntil?"PEAK RESET":""));

}

// CST816 touch controller: a new finger contact resets both peaks.
// If your board uses a different touch controller, this routine will
// simply not register touches; the G-force gauge continues to run.
static bool touchDown(){
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02); // gesture, then finger count at 0x03
  if(Wire.endTransmission(false)!=0)return false;
  if(Wire.requestFrom(TOUCH_ADDR,(uint8_t)2)!=(uint8_t)2)return false;
  Wire.read();
  return (Wire.read()&0x0F)>0;
}
static void checkTouch(){
  static uint32_t lastPoll=0;
  uint32_t now=millis();
  if(now-lastPoll<40)return;
  lastPoll=now;
  bool down=touchDown();
  if(down&&!touchWasDown&&now-lastTapMs>350){
    lastTapMs=now;
    peakLat=peakLong=0;
    for(int i=0;i<TRAIL;i++){histX[i]=0;histY[i]=0;}
    resetNoticeUntil=now+900;
    Serial.println("Peak values reset by touch");
  }
  touchWasDown=down;
}

void setup(){
  Serial.begin(115200);
  pinMode(5,OUTPUT);digitalWrite(5,HIGH);
  if(!gfx->begin())Serial.println("LCD init failed");
  gfx->setRotation(SCREEN_ROTATION);
  gfx->fillScreen(0x0000);
  lv_init();
  lv_disp_draw_buf_init(&lvDraw,drawBuf,nullptr,W*24);
  lv_disp_drv_init(&lvDriver);
  lvDriver.hor_res=W;lvDriver.ver_res=H;
  lvDriver.flush_cb=flush_cb;lvDriver.draw_buf=&lvDraw;
  lv_disp_drv_register(&lvDriver);
  makeUI();
  Wire.begin(11,10);Wire.setClock(400000);
  imuOK=initIMU();
  Serial.printf("QMI8658: %s\n",imuOK?"OK":"NOT FOUND");
  // Keep USB down, display stationary, during the first ~0.8 seconds.
  if(imuOK){
    float sx=0,sy=0,z;int count=0;
    for(int i=0;i<40;i++){
      float x,y;
      if(readAccel(x,y,z)){sx+=x;sy+=y;count++;}
      delay(20);
    }
    if(count){zeroX=sx/count;zeroY=sy/count;}
  }
}
void loop(){
  static uint32_t tick=millis(),sample=0;
  uint32_t now=millis();
  lv_tick_inc(now-tick);tick=now;
  if(now-sample>=50){
    sample=now;
    float x=0,y=0,z=0,gx=0,gy=0;
    if(imuOK&&readAccel(x,y,z)){
      x-=zeroX;y-=zeroY;
      gx=LAT_SIGN*y;gy=LONG_SIGN*x;
    }
    updateUI(gx,gy);
  }
  checkTouch();
  lv_timer_handler();
  delay(5);
}

#include "status_bar.hpp"
#include "suite.hpp"
#include "lvgl.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
namespace passport {
namespace {
constexpr uint32_t bg=0x021018,green=0x4bffc4,white=0xeaf4ff,line=0x103d51;
lv_obj_t *root,*network,*battery;
uint32_t drawnKey=UINT32_MAX;  // inputs the draw callback last painted
const Runtime* current;
void rect(lv_layer_t* layer,int x,int y,int w,int h,uint32_t color,int radius=0,uint32_t border=0){
    lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.bg_color=lv_color_hex(color);d.bg_opa=LV_OPA_COVER;d.radius=radius;
    if(border){d.border_width=1;d.border_color=lv_color_hex(border);d.border_opa=LV_OPA_COVER;}
    const lv_area_t area={x,y,x+w-1,y+h-1};lv_draw_rect(layer,&d,&area);
}
void stroke(lv_layer_t* layer,int x,int y,int x2,int y2){
    lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);d.p1={static_cast<lv_value_precise_t>(x),static_cast<lv_value_precise_t>(y)};d.p2={static_cast<lv_value_precise_t>(x2),static_cast<lv_value_precise_t>(y2)};d.width=2;d.color=lv_color_hex(green);d.opa=LV_OPA_COVER;lv_draw_line(layer,&d);
}
void draw(lv_event_t* event){
    if(!current)return;
    auto* layer=lv_event_get_layer(event);const auto& m=current->model;
    rect(layer,8,34,224,1,line);
    if(m.wifiConnected||(!m.pppConnected&&!m.bleConnected)){
        const int bars=m.wifiConnected?(m.wifiSignalKnown?(m.wifiRssi>=-55?4:m.wifiRssi>=-67?3:m.wifiRssi>=-75?2:1):1):0;
        for(int n=0;n<4;++n)rect(layer,10+n*6,27-(n+1)*4,4,(n+1)*4,n<bars?green:line,1);
    }else if(m.pppConnected){rect(layer,11,10,16,12,green,2);stroke(layer,19,23,19,28);stroke(layer,15,28,23,28);}
    else{stroke(layer,19,8,19,29);stroke(layer,19,8,26,14);stroke(layer,26,14,12,24);stroke(layer,12,13,26,24);stroke(layer,26,24,19,29);}
    rect(layer,206,10,23,13,bg,2,white);rect(layer,229,14,3,5,white,1);
    if(m.batteryKnown&&m.batteryPercent)rect(layer,209,13,std::max(1,int(m.batteryPercent)*17/100),7,m.batteryPercent<=15?0xff6868:green,1);
}
}
lv_obj_t* statusBarInit(lv_obj_t* screen){
    root=lv_obj_create(screen);lv_obj_remove_style_all(root);lv_obj_set_pos(root,0,0);lv_obj_set_size(root,240,35);lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root,lv_color_hex(bg),0);lv_obj_set_style_bg_opa(root,LV_OPA_COVER,0);lv_obj_add_event_cb(root,draw,LV_EVENT_DRAW_MAIN,nullptr);
    network=lv_label_create(root);lv_obj_set_pos(network,39,10);lv_obj_set_width(network,100);lv_obj_set_style_text_font(network,&lv_font_montserrat_14,0);lv_obj_set_style_text_color(network,lv_color_hex(green),0);lv_label_set_long_mode(network,LV_LABEL_LONG_DOT);
    battery=lv_label_create(root);lv_obj_set_pos(battery,141,8);lv_obj_set_width(battery,62);lv_obj_set_style_text_font(battery,&lv_font_montserrat_20,0);lv_obj_set_style_text_color(battery,lv_color_hex(white),0);lv_obj_set_style_text_align(battery,LV_TEXT_ALIGN_RIGHT,0);lv_label_set_long_mode(battery,LV_LABEL_LONG_DOT);
    drawnKey=UINT32_MAX;return root;
}
void statusBarRender(const Runtime& runtime){
    current=&runtime;const auto& m=runtime.model;
    const char* connection=m.wifiConnected?(m.bleConnected?"WiFi + BT":"WiFi"):m.pppConnected?"USB / PPP":m.bleConnected?"BLE":m.wifiSetup?"SETUP":"OFFLINE";
    if(strcmp(lv_label_get_text(network),connection))lv_label_set_text(network,connection);
    char text[12];if(m.batteryKnown)snprintf(text,sizeof(text),"%u%%",m.batteryPercent);else snprintf(text,sizeof(text),"--%%");
    if(strcmp(lv_label_get_text(battery),text))lv_label_set_text(battery,text);
    const int rssi=m.wifiSignalKnown?(m.wifiRssi>=-55?4:m.wifiRssi>=-67?3:m.wifiRssi>=-75?2:1):0;
    const uint32_t key=m.wifiConnected|m.pppConnected<<1|m.bleConnected<<2|m.batteryKnown<<3|rssi<<4|uint32_t(m.batteryPercent)<<8;
    if(key!=drawnKey){drawnKey=key;lv_obj_invalidate(root);}
}
}

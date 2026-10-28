#include "home_ui.hpp"
#include "identity.hpp"
#include "lvgl.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace passport {
namespace {
constexpr uint32_t bg=0x021018,green=0x4bffc4,white=0xeaf4ff,muted=0x9bbbd1,line=0x103d51;
lv_obj_t *root,*clockHour,*dateLabel,*cityLabel,*conditionLabel,*warnBadge;
char shownWarning[16];int shownLevel=-2;  // what the badge was last set to
int drawnIcon=-1;  // the icon root's draw callback last painted
void setText(lv_obj_t* obj,const char* text){if(strcmp(lv_label_get_text(obj),text))lv_label_set_text(obj,text);}
lv_obj_t *values[4],*captions[4];
lv_obj_t *weatherRoot,*cardRoot,*photoRoot;
uint32_t shownIdentity;
const Runtime* current;
lv_obj_t* text(lv_obj_t* parent,int x,int y,int width,const lv_font_t* font,uint32_t color){
    auto* obj=lv_label_create(parent);lv_obj_set_pos(obj,x,y);lv_obj_set_size(obj,width,LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(obj,font,0);lv_obj_set_style_text_color(obj,lv_color_hex(color),0);
    lv_label_set_long_mode(obj,LV_LABEL_LONG_DOT);return obj;
}
void rect(lv_layer_t* layer,int x,int y,int width,int height,uint32_t color,int radius=0,uint32_t border=0){
    lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.bg_color=lv_color_hex(color);d.bg_opa=LV_OPA_COVER;d.radius=radius;
    if(border){d.border_width=1;d.border_color=lv_color_hex(border);d.border_opa=LV_OPA_COVER;}
    const lv_area_t area={x,y,x+width-1,y+height-1};lv_draw_rect(layer,&d,&area);
}
void stroke(lv_layer_t* layer,int x,int y,int x2,int y2,uint32_t color,int width=2){
    lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);d.p1={static_cast<lv_value_precise_t>(x),static_cast<lv_value_precise_t>(y)};d.p2={static_cast<lv_value_precise_t>(x2),static_cast<lv_value_precise_t>(y2)};d.width=width;d.color=lv_color_hex(color);d.opa=LV_OPA_COVER;lv_draw_line(layer,&d);
}
void drop(lv_layer_t* layer,int x,int y,int scale,uint32_t color){
    for(int row=0;row<8;++row){const int half=row<5?row+1:8-row;rect(layer,x+(5-half)*scale,y+row*scale,half*2*scale,scale,color);}
}
void thermometer(lv_layer_t* layer,int x,int y,uint32_t color){
    rect(layer,x+4,y,6,17,color,3);rect(layer,x,y+12,14,14,color,7);rect(layer,x+6,y+3,2,14,bg,1);
}
void sun(lv_layer_t* layer,int x,int y){
    rect(layer,x+8,y+8,20,20,0xffd575,10);
    for(int n=0;n<4;++n){const int offset=n*10;stroke(layer,x+offset,y+2,x+offset+3,y+5,0xffca60);stroke(layer,x+offset,y+31,x+offset+3,y+34,0xffca60);}
    stroke(layer,x+1,y+14,x+5,y+14,0xffca60);stroke(layer,x+31,y+14,x+35,y+14,0xffca60);
}
void weather(lv_layer_t* layer,WeatherIcon icon){
    const int x=179,y=52;
    if(icon==WeatherIcon::Unknown){rect(layer,x+8,y+13,30,17,line,8);return;}
    if(icon==WeatherIcon::Sun){sun(layer,x+3,y+2);return;}
    if(icon==WeatherIcon::PartlyCloudy)sun(layer,x-5,y-4);
    // Pixel steps keep the small cloud legible without a bitmap or canvas cache.
    rect(layer,x+2,y+18,44,18,0x62aafa,3);rect(layer,x+7,y+10,17,23,0xa4d2ff,4);
    rect(layer,x+20,y+3,16,29,0xcbe7ff,4);rect(layer,x+32,y+12,12,20,0x96c9ff,3);
    rect(layer,x+5,y+29,38,8,0x4c99f3,2);
    if(icon==WeatherIcon::Rain){stroke(layer,x+15,y+41,x+11,y+49,0x00baf2,3);stroke(layer,x+34,y+41,x+30,y+49,0x00baf2,3);}
    if(icon==WeatherIcon::Snow){for(int n=0;n<2;++n){int sx=x+14+n*19;stroke(layer,sx-3,y+45,sx+3,y+45,white,2);stroke(layer,sx,y+41,sx,y+49,white,2);}}
    if(icon==WeatherIcon::Thunder){stroke(layer,x+25,y+35,x+18,y+43,0xffce64,4);stroke(layer,x+18,y+43,x+27,y+43,0xffce64,4);stroke(layer,x+27,y+43,x+21,y+51,0xffce64,4);}
    if(icon==WeatherIcon::Fog){stroke(layer,x+3,y+42,x+44,y+42,muted,2);stroke(layer,x+9,y+48,x+38,y+48,muted,2);}
}
void draw(lv_event_t* event){
    auto* layer=lv_event_get_layer(event);if(!current||current->model.homeScreen!=HomeScreen::Weather)return;const auto& m=current->model;
    rect(layer,8,163,224,1,line);
    weather(layer,m.weatherReading.known?weatherIcon(m.weatherReading.code):WeatherIcon::Unknown);
    rect(layer,14,135,12,12,muted,6);rect(layer,18,139,4,4,bg,2);stroke(layer,17,145,20,151,muted,3);stroke(layer,23,145,20,151,muted,3);
    rect(layer,8,173,108,53,0x201617,7,0x663434);rect(layer,123,173,109,53,0x061a25,7,0x145070);
    rect(layer,8,233,108,53,0x041b28,7,0x124d6c);rect(layer,123,233,109,53,0x202014,7,0x5a4a1c);
    thermometer(layer,16,184,0xff8c4c);thermometer(layer,131,184,0xee6373);
    drop(layer,13,244,2,0x09b8ed);
    rect(layer,132,246,14,17,0x7fe152,7);stroke(layer,132,265,144,246,bg,2);
}
lv_obj_t* panel(lv_obj_t* parent,int x,int y,int width,int height,uint32_t color,int radius,uint32_t border){
    auto* obj=lv_obj_create(parent);lv_obj_remove_style_all(obj);lv_obj_set_pos(obj,x,y);lv_obj_set_size(obj,width,height);lv_obj_remove_flag(obj,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(obj,lv_color_hex(color),0);lv_obj_set_style_bg_opa(obj,LV_OPA_COVER,0);lv_obj_set_style_radius(obj,radius,0);
    if(border){lv_obj_set_style_border_width(obj,1,0);lv_obj_set_style_border_color(obj,lv_color_hex(border),0);}
    return obj;
}
bool ascii(const char* value){for(;*value;++value)if(static_cast<unsigned char>(*value)>127)return false;return true;}
void placeholder(lv_obj_t* parent,int y,const char* title,const char* hint){
    auto* t=text(parent,12,y,216,LV_FONT_DEFAULT,white);lv_obj_set_style_text_align(t,LV_TEXT_ALIGN_CENTER,0);lv_label_set_text(t,title);
    auto* h=text(parent,20,y+28,200,LV_FONT_DEFAULT,muted);lv_label_set_long_mode(h,LV_LABEL_LONG_WRAP);lv_obj_set_style_text_align(h,LV_TEXT_ALIGN_CENTER,0);lv_label_set_text(h,hint);
}
// Passport-style card: avatar on the left, name/title/number on the right, motto below.
void initCard(lv_obj_t* screen){
    const auto& id=identity();
    cardRoot=panel(screen,0,35,240,257,bg,0,0);
    auto* card=panel(cardRoot,8,7,224,245,0x062433,12,0x1d5a73);
    auto* brand=text(card,12,8,140,&lv_font_montserrat_14,green);lv_label_set_text(brand,"AI PASSPORT");lv_obj_set_style_text_letter_space(brand,2,0);
    auto* kind=text(card,150,8,62,LV_FONT_DEFAULT,muted);lv_obj_set_style_text_align(kind,LV_TEXT_ALIGN_RIGHT,0);lv_label_set_text(kind,"身份卡");lv_obj_set_y(kind,5);
    panel(card,12,30,200,1,line,0,0);
    if(!id.valid){placeholder(card,90,id.busy?"正在更新身份卡":"尚未导入身份卡",id.busy?"请保持手机连接 Passport 热点":"设置 → WiFi 配置，用手机网页填写姓名、头像和签名");return;}
    const auto& h=id;
    auto* frame=panel(card,11,40,kAvatarWidth+2,kAvatarHeight+2,0x0b3446,6,0x2d7d96);
    if(id.hasAvatar){auto* avatar=lv_image_create(frame);lv_image_set_src(avatar,&id.avatar);lv_obj_set_pos(avatar,0,0);}
    else{auto* initial=text(frame,0,60,kAvatarWidth,&lv_font_montserrat_48,green);lv_obj_set_style_text_align(initial,LV_TEXT_ALIGN_CENTER,0);char first[2]{h.name[0]&&ascii(h.name)?h.name[0]:'?',0};lv_label_set_text(initial,first);}
    const int x=118,width=96;
    auto caption=[&](int y,const char* value){auto* c=text(card,x,y,width,&lv_font_montserrat_14,muted);lv_label_set_text(c,value);};
    caption(42,"NAME");
    auto* name=text(card,x,58,width,ascii(h.name)?&lv_font_montserrat_24:LV_FONT_DEFAULT,white);lv_label_set_text(name,h.name);
    caption(94,"TITLE");
    auto* title=text(card,x,110,width,LV_FONT_DEFAULT,white);lv_obj_set_height(title,40);lv_label_set_long_mode(title,LV_LABEL_LONG_WRAP);lv_label_set_text(title,h.title);
    caption(156,"NO.");
    auto* number=text(card,x,172,width,&lv_font_montserrat_14,green);lv_label_set_text(number,id.number);
    panel(card,12,204,200,1,line,0,0);
    auto* motto=text(card,12,209,200,LV_FONT_DEFAULT,muted);lv_obj_set_height(motto,20);lv_label_set_text(motto,h.motto);
}
void initPhoto(lv_obj_t* screen){
    const auto& id=identity();
    photoRoot=panel(screen,0,0,240,320,bg,0,0);
    if(id.valid&&id.hasPhoto){auto* photo=lv_image_create(photoRoot);lv_image_set_src(photo,&id.photo);lv_obj_set_pos(photo,0,0);}
    else placeholder(photoRoot,130,id.busy?"正在更新照片":"尚未导入照片",id.busy?"请保持手机连接 Passport 热点":"设置 → WiFi 配置，用手机网页上传照片");
}
}
void homeUiInit(lv_obj_t* screen){
    root=lv_obj_create(screen);lv_obj_remove_style_all(root);lv_obj_set_pos(root,0,0);lv_obj_set_size(root,240,320);lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root,lv_color_hex(bg),0);lv_obj_set_style_bg_opa(root,LV_OPA_COVER,0);lv_obj_add_event_cb(root,draw,LV_EVENT_DRAW_MAIN,nullptr);
    weatherRoot=root;
    clockHour=text(root,10,47,135,&lv_font_montserrat_48,green);
    dateLabel=text(root,12,108,161,LV_FONT_DEFAULT,muted);cityLabel=text(root,33,133,123,LV_FONT_DEFAULT,white);
    lv_obj_set_width(cityLabel,LV_SIZE_CONTENT);
    // 气象预警: the highest kind in force, on its level's colour, right after the city.
    warnBadge=text(root,0,133,90,LV_FONT_DEFAULT,bg);lv_obj_set_width(warnBadge,LV_SIZE_CONTENT);lv_obj_set_style_bg_opa(warnBadge,LV_OPA_COVER,0);
    lv_obj_set_style_radius(warnBadge,4,0);lv_obj_set_style_pad_hor(warnBadge,4,0);lv_obj_add_flag(warnBadge,LV_OBJ_FLAG_HIDDEN);shownLevel=-2;
    conditionLabel=text(root,161,113,73,LV_FONT_DEFAULT,white);lv_label_set_long_mode(conditionLabel,LV_LABEL_LONG_WRAP);lv_obj_set_style_text_align(conditionLabel,LV_TEXT_ALIGN_CENTER,0);
    for(unsigned i=0;i<4;++i){
        const int x=i%2?151:36,y=i/2?239:179;
        values[i]=text(root,x,y,i%2?77:79,i==1?&lv_font_montserrat_20:&lv_font_montserrat_24,white);
        captions[i]=text(root,i%2?130:35,y+27,i%2?100:78,LV_FONT_DEFAULT,muted);
    }
    lv_label_set_text(captions[0],"温度°C");lv_label_set_text(captions[1],"最低 / 最高");lv_label_set_text(captions[2],"湿度");lv_label_set_text(captions[3],"空气 AQI");
    cardRoot=photoRoot=nullptr;shownIdentity=identityRevision();drawnIcon=-1;
}
void homeUiRender(const Runtime& runtime){
    current=&runtime;const auto& m=runtime.model;const auto& w=m.weatherReading;char buffer[96];
    // Only the visible screen owns objects; card and photo are rebuilt on entry.
    const auto screen=m.homeScreen;auto* display=lv_obj_get_screen(weatherRoot);
    // A new upload invalidates the flash mapping: drop image objects before identity() remaps.
    const bool changed=identityRevision()!=shownIdentity;shownIdentity=identityRevision();
    if((screen!=HomeScreen::Card||changed)&&cardRoot){lv_obj_delete(cardRoot);cardRoot=nullptr;}
    if((screen!=HomeScreen::Photo||changed)&&photoRoot){lv_obj_delete(photoRoot);photoRoot=nullptr;}
    if(screen==HomeScreen::Card&&!cardRoot){initCard(display);lv_obj_move_background(cardRoot);}
    if(screen==HomeScreen::Photo&&!photoRoot){initPhoto(display);lv_obj_move_background(photoRoot);}
    if(screen==HomeScreen::Weather)lv_obj_remove_flag(weatherRoot,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(weatherRoot,LV_OBJ_FLAG_HIDDEN);
    if(screen!=HomeScreen::Weather)return; // Static screens are not redrawn every second.
    const auto stamp=time(nullptr);tm local{};const bool clockReady=m.clockValid&&stamp>1700000000;localtime_r(&stamp,&local);
    if(clockReady){strftime(buffer,sizeof(buffer),"%H:%M",&local);setText(clockHour,buffer);
        const char* days[]={"周日","周一","周二","周三","周四","周五","周六"};char date[24];strftime(date,sizeof(date),"%Y-%m-%d",&local);snprintf(buffer,sizeof(buffer),"%s %s",date,days[local.tm_wday]);setText(dateLabel,buffer);
    }else{setText(clockHour,"--:--");setText(dateLabel,"等待网络校时");}
    setText(cityLabel,w.known?w.city:"请先选择城市");setText(conditionLabel,w.known?w.condition:m.livePhase==LivePhase::Loading?"更新中":"天气未知");
    // Style writes repaint even when unchanged, so the badge is only touched when the warning changes.
    const int level=w.known&&*w.warning?w.warningLevel:-1;
    if(level!=shownLevel||strcmp(shownWarning,w.warning)){
        shownLevel=level;snprintf(shownWarning,sizeof(shownWarning),"%s",w.warning);
        if(level>=0){snprintf(buffer,sizeof(buffer),strlen(w.warning)<=6?"%s预警":"%s",w.warning);setText(warnBadge,buffer);
            lv_obj_set_style_bg_color(warnBadge,lv_color_hex(kWarningColors[level]),0);lv_obj_set_style_text_color(warnBadge,lv_color_hex(level==1||level==2?bg:white),0);
            lv_obj_update_layout(cityLabel);lv_obj_set_x(warnBadge,33+lv_obj_get_width(cityLabel)+6);lv_obj_remove_flag(warnBadge,LV_OBJ_FLAG_HIDDEN);}
        else lv_obj_add_flag(warnBadge,LV_OBJ_FLAG_HIDDEN);
    }
    if(w.known)snprintf(buffer,sizeof(buffer),"%.1f°",w.temperature);else snprintf(buffer,sizeof(buffer),"--°");setText(values[0],buffer);
    if(w.known)snprintf(buffer,sizeof(buffer),"%.0f°/%.0f°",w.minimum,w.maximum);else snprintf(buffer,sizeof(buffer),"--/--");
    lv_obj_set_x(values[1],146);lv_obj_set_width(values[1],83);setText(values[1],buffer);
    if(w.known)snprintf(buffer,sizeof(buffer),"%.0f%%",w.humidity);else snprintf(buffer,sizeof(buffer),"--%%");setText(values[2],buffer);
    if(w.airKnown)snprintf(buffer,sizeof(buffer),"%.0f",w.aqi);else snprintf(buffer,sizeof(buffer),"--");setText(values[3],buffer);
    lv_obj_set_style_text_color(values[3],lv_color_hex(w.airKnown&&w.aqi>150?0xffbf77:white),0);
    // The hand-drawn layer depends only on the weather icon; repainting it every second redrew the whole screen.
    const int icon=static_cast<int>(w.known?weatherIcon(w.code):WeatherIcon::Unknown);
    if(icon!=drawnIcon){drawnIcon=icon;lv_obj_invalidate(root);}
}
}

#include "render/survival_ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "gfx/gl.h"

namespace zl {
namespace {
constexpr u32 ink = pack_color(216,207,180), muted = pack_color(153,146,125), gold = pack_color(191,163,94);
constexpr u32 panel = pack_color(29,27,23,246), slot = pack_color(48,44,35), edge = pack_color(99,84,61);
// Five-column bitmap alphabet. Lowercase is deliberately rendered in small
// capitals for a consistent, legible strategy-game HUD at every UI scale.
const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,:/-+[]()!?<>=_";
const unsigned char glyphs[][5] = {
 {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},{127,73,73,73,65},{127,9,9,9,1},
 {62,65,73,73,122},{127,8,8,8,127},{0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
 {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},{62,65,81,33,94},{127,9,25,41,70},
 {70,73,73,73,49},{1,1,127,1,1},{63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
 {7,8,112,8,7},{97,81,73,69,67},{62,81,73,69,62},{0,66,127,64,0},{98,81,73,73,70},{34,65,73,73,54},
 {24,20,18,127,16},{39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},{54,73,73,73,54},{6,73,73,41,30},
 {0,0,0,0,0},{0,96,96,0,0},{0,80,48,0,0},{0,54,54,0,0},{32,16,8,4,2},{8,8,8,8,8},{8,8,62,8,8},
 {0,127,65,65,0},{0,65,65,127,0},{0,28,34,65,0},{0,65,34,28,0},{0,0,95,0,0},{2,1,81,9,6},
 {8,20,34,65,0},{0,65,34,20,8},{20,20,20,20,20},{64,64,64,64,64}
};
}

void SurvivalUI::init(const char* asset_dir) {
    batch_.init();
    reload_art(asset_dir);
}
void SurvivalUI::reload_art(const char* asset_dir) {
#ifdef ZL_ASSET_DIR
    if (!asset_dir) asset_dir=ZL_ASSET_DIR;
#endif
    Image atlas(256,128);
    atlas.at(127,63) = {255,255,255,255};
    for (size_t g = 0; g < std::strlen(alphabet); ++g)
        for (int x = 0; x < 5; ++x) for (int y = 0; y < 7; ++y)
            if (glyphs[g][x] & (1 << y)) atlas.at(static_cast<int>(g%16)*8+x, static_cast<int>(g/16)*8+y) = {255,255,255,255};
    const char* names[]={"wood","stone","fiber","rope","axe","pickaxe","chest"};
    for(int i=0;i<7;++i) {
        Image icon;
        const std::string path=std::string(asset_dir?asset_dir:"")+"/icons/"+names[i]+".png";
        if(!read_png(path.c_str(),icon)||icon.width!=32||icon.height!=32) {
            ZL_LOG("art: missing or invalid inventory icon %s",path.c_str());
            continue;
        }
        for(int y=0;y<32;++y)for(int x=0;x<32;++x) {
            Color c=icon.at(x,y);const u32 a=c.a;
            c.r=static_cast<u8>((c.r*a+127)/255);c.g=static_cast<u8>((c.g*a+127)/255);c.b=static_cast<u8>((c.b*a+127)/255);
            atlas.at(i*32+x,64+y)=c;
        }
    }
    atlas_.create(atlas, TextureFilter::Nearest, TextureWrap::Clamp);
}
void SurvivalUI::rect(float x, float y, float w, float h, u32 color) {
    SpriteInstance s{};
    s.x = x*scale_; s.y = y*scale_; s.w = w*scale_; s.h = h*scale_;
    s.u0 = s.u1 = static_cast<u16>(127.5f/256*65535); s.v0 = s.v1 = static_cast<u16>(63.5f/128*65535); s.color = color;
    sprites_.push_back(s);
}
void SurvivalUI::text(float x, float y, const std::string& value, u32 color, float size) {
    for (char c : value) {
        const char* found = std::strchr(alphabet, std::toupper(static_cast<unsigned char>(c)));
        if (found) {
            const size_t g = static_cast<size_t>(found-alphabet);
            SpriteInstance s{};
            s.x = x*scale_; s.y = y*scale_; s.w = 5*size*scale_; s.h = 7*size*scale_;
            s.u0 = static_cast<u16>((g%16)*8*65535/256); s.v0 = static_cast<u16>((g/16)*8*65535/128);
            s.u1 = static_cast<u16>(((g%16)*8+5)*65535/256); s.v1 = static_cast<u16>(((g/16)*8+7)*65535/128);
            s.color = color; sprites_.push_back(s);
        }
        x += 6*size;
    }
}
void SurvivalUI::icon(float x, float y, Item item, float size) {
    const int index=static_cast<int>(item)-1;
    if(index<0||index>=7)return;
    SpriteInstance s{};
    s.x=x*scale_;s.y=y*scale_;s.w=size*scale_;s.h=size*scale_;
    s.u0=static_cast<u16>(index*32*65535/256);s.u1=static_cast<u16>((index+1)*32*65535/256);
    s.v0=static_cast<u16>(64*65535/128);s.v1=static_cast<u16>(96*65535/128);s.color=0xFFFFFFFFu;
    sprites_.push_back(s);
}
bool SurvivalUI::hit(float x,float y,float w,float h) const { return mouse_.x>=x && mouse_.y>=y && mouse_.x<x+w && mouse_.y<y+h; }
bool SurvivalUI::blocks_world(Vec2 mouse, Vec2 viewport) const {
    const float scale=std::min(viewport.x/1024,viewport.y/720);
    return open || mouse.y < 80*scale || mouse.y > viewport.y-116*scale;
}
bool SurvivalUI::close(Survival& game) {
    if (hand.count && !game.bag.add(hand.item,hand.count)) {
        if (chest < 0 || chest >= static_cast<int>(game.chests.size()) || !game.chests[static_cast<size_t>(chest)].inventory.add(hand.item,hand.count)) {
            game.message="Place the held stack in an empty slot first"; game.message_time=3; return false;
        }
    }
    hand={}; open=false; chest=-1; return true;
}
void SurvivalUI::update(const Input& input, Vec2 viewport, Survival& game) {
    scale_=std::min(viewport.x/1024,viewport.y/720);
    mouse_=input.mouse/scale_;
    if(placing&&input.pressed(Key::T))placement_facing=static_cast<u8>((placement_facing+1)%4);
    if(!open) return;
    const bool left=input.pressed(MouseButton::Left), right=input.pressed(MouseButton::Right);
    if(!left && !right) return;
    const bool shift=input.down(Key::LeftShift);
    const auto grid=[&](Inventory& inv,float x,float y,int cols,Inventory* other) {
        for(int i=0;i<static_cast<int>(inv.slots().size());++i) if(hit(x+static_cast<float>(i%cols)*66,y+static_cast<float>(i/cols)*66,60,60)) {
            if(shift && other && !hand.count) inv.transfer(*other,i,right?1:50);
            else inv.click(i,hand,right);
        }
    };
    Inventory* storage=chest>=0 && chest<static_cast<int>(game.chests.size()) ? &game.chests[static_cast<size_t>(chest)].inventory : nullptr;
    grid(game.bag,38,175,5,storage);
    if(storage) grid(*storage,433,175,6,&game.bag);
    else for(int r=0;r<4;++r) if(left && hit(432,169+static_cast<float>(r)*76,535,68) && !hand.count) {
        if(!game.start_craft(r,shift?5:1)) {game.message="Need ingredients, free output space, or finish current craft"; game.message_time=3;}
    }
    if(left && !hand.count && hit(38,487,324,37) && game.bag.count(Item::Chest)>0) { if(close(game)) placing=true; }
}
void SurvivalUI::flush(Vec2 viewport) {
    if(sprites_.empty()) return;
    auto* out=batch_.begin(static_cast<u32>(sprites_.size()));
    std::copy(sprites_.begin(),sprites_.end(),out);
    Camera2D camera;camera.viewport=viewport;camera.center=viewport*0.5f;camera.zoom=1;
    glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);
    batch_.draw(static_cast<u32>(sprites_.size()),camera,atlas_);
    glDepthMask(GL_TRUE);sprites_.clear();
}
void SurvivalUI::marker(Vec2 screen, Vec2 viewport, bool valid,float progress) {
    const float save=scale_;scale_=1;
    const u32 color=valid?gold:pack_color(163,78,62);
    for(int i=0;i<24;++i) {const float a=static_cast<float>(i)*6.28318f/24;rect(screen.x+std::cos(a)*18,screen.y+std::sin(a)*9,3,2,color);}
    if(progress>0) {rect(screen.x-24,screen.y+17,48,5,panel);rect(screen.x-24,screen.y+17,48*progress,5,gold);}
    flush(viewport);scale_=save;
}
void SurvivalUI::impact(Vec2 screen, Vec2 viewport, Item item, float age) {
    if(age<0||age>0.65f)return;
    const float save=scale_;scale_=1;
    const u8 alpha=static_cast<u8>(255*(1-age/0.65f));
    const u32 color=item==Item::Stone?pack_color(191,185,163,alpha):pack_color(167,137,88,alpha);
    for(int i=0;i<7;++i) {
        const float vx=(static_cast<float>(i)-3)*20;
        rect(screen.x+vx*age,screen.y-18-age*(30+static_cast<float>(i%3)*20)+age*age*100,3,3,color);
    }
    text(screen.x-20,screen.y-34-age*40,"+1 "+std::string(item_name(item)),pack_color(216,207,180,alpha),1.4f);
    flush(viewport);scale_=save;
}
void SurvivalUI::draw(Vec2 viewport,const Input& input,const Survival& game,const char* prompt,bool follow) {
    scale_=std::min(viewport.x/1024,viewport.y/720);mouse_=input.mouse/scale_;
    const float width=viewport.x/scale_,height=viewport.y/scale_;
    rect(18,18,640,59,panel);rect(18,18,3,59,gold);
    text(33,30,"ZILLIONS / THE FIRST CAMP",gold,1.7f);
    text(33,55,follow?"WASD move  /  Hold left mouse: gather":"MANAGEMENT / Space: return to your character",ink,1.5f);
    rect(width-316,18,298,59,panel);
    text(width-300,30,"I inventory   B crafting",ink,1.6f);
    text(width-300,55,"Space view   Q/E rotate",muted,1.5f);
    rect(18,height-113,width-36,95,panel);
    const Item summary[]={Item::Wood,Item::Stone,Item::Fiber,Item::Rope};
    for(int i=0;i<4;++i) {const float x=32+static_cast<float>(i)*132;icon(x,height-103,summary[i],28);text(x+35,height-95,std::to_string(game.bag.count(summary[i])),ink,2);}
    icon(width-270,height-104,Item::Axe,30);text(width-233,height-94,game.axe?"READY":"MISSING",game.axe?gold:muted,1.5f);
    icon(width-140,height-104,Item::Pickaxe,30);text(width-103,height-94,game.pickaxe?"READY":"MISSING",game.pickaxe?gold:muted,1.5f);
    const char* facing_names[]={"S","W","N","E"};
    const std::string status=placing?std::string("PLACE CHEST / ")+facing_names[placement_facing%4]+" / T rotate / Left place / Right cancel":(prompt?prompt:"Gather loose materials, craft tools, then establish a camp.");
    text(33,height-62,status.substr(0,102),ink,1.5f);
    if(game.message_time>0) text(33,height-40,game.message.substr(0,102),gold,1.5f);
    if(game.crafting()>=0) {rect(300,height-135,424,12,slot);rect(300,height-135,424*game.craft_progress(),12,gold);text(300,height-153,"CRAFTING "+std::string(item_name(recipes()[static_cast<size_t>(game.crafting())].output))+" x"+std::to_string(game.crafting_left()),ink,1.5f);}
    if(open) {
        rect(20,100,365,444,panel);rect(412,100,580,444,panel);rect(20,100,365,2,gold);rect(412,100,580,2,gold);
        text(38,121,"SATCHEL / 20 SLOTS",ink,2);text(38,151,"Click: move / Right: split or place one",muted,1.25f);
        const auto grid=[&](const Inventory& inv,float x,float y,int cols) {
            for(int i=0;i<static_cast<int>(inv.slots().size());++i) {
                const float sx=x+static_cast<float>(i%cols)*66,sy=y+static_cast<float>(i/cols)*66;
                rect(sx,sy,60,60,edge);rect(sx+1,sy+1,58,58,hit(sx,sy,60,60)?pack_color(72,65,48):slot);
                const auto& s=inv.slots()[static_cast<size_t>(i)];
                if(s.count) {icon(sx+12,sy+6,s.item,35);text(sx+5,sy+44,std::to_string(s.count),ink,1.6f);if(hit(sx,sy,60,60)) text(38,455,item_name(s.item),gold,1.5f);}
            }
        };
        grid(game.bag,38,175,5);
        rect(38,487,324,37,slot);text(51,499,"PLACE CHEST ["+std::to_string(game.bag.count(Item::Chest))+"]",game.bag.count(Item::Chest)?gold:muted,1.6f);
        if(chest>=0 && chest<static_cast<int>(game.chests.size())) {
            text(433,121,"CAMP CHEST / 24 SLOTS",ink,2);text(433,151,"Shift-click: transfer / B: crafting",muted,1.5f);
            grid(game.chests[static_cast<size_t>(chest)].inventory,433,175,6);
        } else {
            text(433,121,"HAND CRAFTING",ink,2);text(433,151,"Click: craft one / Shift-click: up to five",muted,1.4f);
            for(int r=0;r<4;++r) {
                const auto& recipe=recipes()[static_cast<size_t>(r)];const float y=169+static_cast<float>(r)*76;
                const bool enabled=game.can_craft(r)&&game.crafting()<0;
                rect(432,y,535,68,hit(432,y,535,68)?pack_color(65,58,43):slot);rect(432,y,3,68,enabled?gold:edge);
                icon(444,y+13,recipe.output,36);text(495,y+10,item_name(recipe.output),enabled?ink:muted,1.8f);
                std::string ingredients;
                for(const auto& s:recipe.ingredients) if(s.count) {if(!ingredients.empty()) ingredients+=" / ";ingredients+=std::to_string(s.count)+" "+item_name(s.item);}
                text(495,y+34,ingredients,muted,1.2f);
                const bool owned=(recipe.output==Item::Axe&&game.axe)||(recipe.output==Item::Pickaxe&&game.pickaxe);
                text(495,y+51,owned?"EQUIPPED":(enabled?"READY TO CRAFT":"NEED MATERIALS OR SPACE"),enabled?gold:muted,1.1f);
            }
        }
        text(433,510,"I / ESC close   -   Crafting uses carried supplies",muted,1.25f);
    }
    if(hand.count) {icon(mouse_.x+8,mouse_.y+8,hand.item,32);text(mouse_.x+37,mouse_.y+30,std::to_string(hand.count),ink,1.6f);}
    flush(viewport);
}

} // namespace zl

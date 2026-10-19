#include "tankerkoenig.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace esphome::tankerkoenig {

using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

static int fuel_index(const char *key) {
  for (int i = 0; i < FUELS; ++i)
    if (key && strcmp(key, FUEL_KEYS[i]) == 0) return i;
  return -1;
}

static void copy(char *dst, size_t size, const char *src) {
  snprintf(dst, size, "%s", src ? src : "");
}

// Why the tile has nothing to show; NONE when it has.
enum class Why : uint8_t { NONE, CHOOSE, WAITING, FAILED, ALL_CLOSED };

// The tile. One column: the first station (the cheapest when sorted) with one price large. Two columns or more: a
// header with the fuels and a row per station, its name and address on the left and a price per fuel on the right.
// Home Assistant sends the sensor again at every change; the order, the filter and the colours are worked out again on every tick,
// which for ten stations costs nothing, and ui::set_* change a label only when it differs.
class PricesTile : public tessera::Tile {
 public:
  explicit PricesTile(Tankerkoenig *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    tile_ = c.tile;
    width_ = c.width;
    height_ = c.height;
    read_options(c.options);
    copy(decimal_, sizeof decimal_, std::string(plugin_->text("decimal")).c_str());
    if (!decimal_[0]) copy(decimal_, sizeof decimal_, ".");
    list_ = c.columns >= 2;
    if (list_) make_list(c.parent);
    else make_single(c.parent);
  }

  void on_state(JsonObjectConst data) override {
    const char *wait = data["wait"] | "";
    wait_ = *wait ? Why::FAILED : Why::NONE;
    if (data["state"].isNull() && !*wait) wait_ = Why::CHOOSE;  // no sensor chosen yet
    JsonObjectConst a = data["attributes"];
    JsonArrayConst names = a["station_names"], places = a["station_places"], open = a["station_open"];
    JsonArrayConst prices[FUELS] = {a["e5"], a["e10"], a["diesel"]};
    // The lists come in the order of the template's device list; a station is known again by its name and address,
    // so a price keeps its colour of change across a list that grew or shrank.
    Station before[MAX_STATIONS];
    const int before_count = station_count_;
    memcpy(before, stations_, sizeof before);
    station_count_ = 0;
    for (size_t i = 0; i < names.size() && station_count_ < MAX_STATIONS; ++i) {
      Station &s = stations_[station_count_++];
      memset(&s, 0, sizeof s);
      copy(s.title, sizeof s.title, names[i] | "");
      copy(s.where, sizeof s.where, places[i] | "");
      const int o = open[i] | -1;
      s.state = o > 0 ? Station::OPEN : o == 0 ? Station::CLOSED : Station::UNKNOWN;
      const Station *old = nullptr;
      for (int k = 0; k < before_count; ++k)
        if (strcmp(before[k].title, s.title) == 0 && strcmp(before[k].where, s.where) == 0) old = &before[k];
      bool any = false;
      for (int f = 0; f < FUELS; ++f) {
        const float now = prices[f][i] | 0.0f;
        s.price[f] = now > 0 ? now : 0;
        any |= now > 0;
        if (!old) continue;
        s.trend[f] = old->trend[f];
        // A change of a tenth of a cent or more turns the price's colour until the next change.
        if (now > 0 && old->price[f] > 0 && fabsf(now - old->price[f]) >= 0.0005f)
          s.trend[f] = now > old->price[f] ? 1 : -1;
        if (now <= 0 && s.state == Station::CLOSED) s.price[f] = old->price[f];  // closed: keep the last prices
      }
      if (!any && s.state != Station::CLOSED) s.state = Station::NO_PRICES;
    }
  }

  void on_tick(uint32_t) override {
    const Station *shown[MAX_STATIONS];
    const int count = visible(shown);
    Why why = wait_;
    if (why == Why::NONE && station_count_ == 0) why = Why::WAITING;
    else if (why == Why::NONE && count == 0) why = hide_closed_ ? Why::ALL_CLOSED : Why::WAITING;
    if (list_) paint_list(shown, count, why);
    else paint_single(shown, count, why);
  }

  // A tap opens every station with every fuel.
  void on_tap() override {
    if (station_count_ == 0) return;
    memcpy(plugin_->card_stations, stations_, sizeof stations_);
    plugin_->card_count = station_count_;
    tessera::open_card(plugin_->plugin_id(), "stations", "", tile_);
  }

  // The stations the card starts with, before its first data.
  void load(const Station *stations, int count) {
    station_count_ = std::min(count, MAX_STATIONS);
    memcpy(stations_, stations, sizeof(Station) * station_count_);
    wait_ = Why::NONE;
  }

  void on_theme() override {
    // Every colour is set by role on each tick; a new look only needs the next paint, which follows at once.
    for (int i = 0; i < FUELS; ++i)
      if (head_[i]) ui::set_color(head_[i], theme::MUTED);
    if (note_) ui::set_color(note_, theme::MUTED);
    if (name_) ui::set_color(name_, theme::MUTED);
  }

 private:
  struct Row {
    lv_obj_t *title{}, *where{};
    lv_obj_t *price[FUELS]{}, *tenth[FUELS]{};  // "1.79" and the small "9" beside it
  };

  void read_options(JsonObjectConst o) {
    // The fuels to show, in the order of the price boards at the road: diesel, E10, E5.
    fuel_count_ = 0;
    for (int f : {2, 1, 0})
      if (o[FUEL_KEYS[f]] | true) fuels_[fuel_count_++] = f;
    if (fuel_count_ == 0) fuels_[fuel_count_++] = 2;
    sort_ = fuel_index(o["sort"] | "none");
    cheapest_ = o["cheapest"] | 0;
    hide_closed_ = o["hide_closed"] | false;
    address_ = o["address"] | true;
    status_ = o["status"] | true;
    changes_ = o["changes"] | true;
    // The station in front: a part of its name or address, compared without regard to case.
    copy(front_, sizeof front_, lower(o["front"] | "").c_str());
  }

  // The stations to show, in the order to show them: closed ones left out when asked, sorted by the chosen fuel
  // (stations without that price last), and only the cheapest few when asked.
  int visible(const Station **out) const {
    int n = 0;
    for (int i = 0; i < station_count_; ++i) {
      const Station &s = stations_[i];
      if (hide_closed_ && s.state == Station::CLOSED) continue;
      out[n++] = &s;
    }
    if (sort_ >= 0) {
      const int f = sort_;
      std::stable_sort(out, out + n, [f](const Station *a, const Station *b) {
        const bool ha = a->price[f] > 0 && a->state == Station::OPEN, hb = b->price[f] > 0 && b->state == Station::OPEN;
        if (ha != hb) return ha;
        return ha && a->price[f] < b->price[f];
      });
      if (cheapest_ > 0 && n > cheapest_) n = cheapest_;
    }
    // The station chosen for the front goes first, whatever the order and the filter say; the rest keep their order,
    // and the last one makes way when only the cheapest few are shown.
    if (front_[0]) {
      const Station *pinned = nullptr;
      for (int i = 0; i < station_count_ && !pinned; ++i)
        if (lower(stations_[i].title).find(front_) != std::string::npos ||
            lower(stations_[i].where).find(front_) != std::string::npos)
          pinned = &stations_[i];
      if (pinned) {
        int at = 0;
        while (at < n && out[at] != pinned) ++at;
        if (at == n) {
          const int limit = sort_ >= 0 && cheapest_ > 0 ? cheapest_ : MAX_STATIONS;
          at = std::min(n, limit - 1);
          if (n < limit) ++n;
        }
        for (int i = at; i > 0; --i) out[i] = out[i - 1];
        out[0] = pinned;
      }
    }
    return n;
  }

  // Lowercase, umlauts as their plain vowel and ß as ss, so "Huenger", "Hünger" and "HÜNGER" are the same word.
  static std::string lower(const char *text) {
    std::string out;
    for (const char *p = text ? text : ""; *p; ++p) {
      const unsigned char c = static_cast<unsigned char>(*p);
      if (c == 0xCC && static_cast<unsigned char>(p[1]) == 0x88) {  // a combining diaeresis
        ++p;
        continue;
      }
      if (c == 0xC3 && p[1]) {
        const unsigned char n = static_cast<unsigned char>(p[1]);
        const char *plain = n == 0xA4 || n == 0x84 ? "a" : n == 0xB6 || n == 0x96 ? "o" : n == 0xBC || n == 0x9C ? "u"
                            : n == 0x9F ? "ss" : nullptr;
        if (plain) {
          out += plain;
          ++p;
          continue;
        }
      }
      out += static_cast<char>(std::tolower(c));
    }
    return out;
  }

  // The fuel a single cell shows: the one it sorts by, else the first one chosen.
  int main_fuel() const { return sort_ >= 0 ? sort_ : fuels_[0]; }

  // 1.799 as "1.79" and "9", with the screen's decimal sign; "-" for a price there is none of.
  void price_parts(float price, std::string &big, std::string &tenth) const {
    if (price <= 0) {
      big = "-";
      tenth.clear();
      return;
    }
    const long milli = lroundf(price * 1000.0f);
    char text[16];
    snprintf(text, sizeof text, "%ld%s%02ld", milli / 1000, decimal_, (milli % 1000) / 10);
    big = text;
    tenth = std::to_string(milli % 10);
  }

  // The colour of a price: red when it went up, green when it went down, muted at a closed station.
  void tone_price(lv_obj_t *label, const Station &s, int f, theme::Role normal) const {
    if (s.state == Station::CLOSED) ui::set_tone(label, Tone::NORMAL, theme::MUTED);
    else if (changes_ && s.trend[f] > 0) ui::set_tone(label, Tone::ALERT, normal);
    else if (changes_ && s.trend[f] < 0) ui::set_tone(label, Tone::GOOD, normal);
    else ui::set_tone(label, Tone::NORMAL, normal);
  }

  std::string why_text(Why why) const {
    switch (why) {
      case Why::CHOOSE: return plugin_->text("choose");
      case Why::WAITING: return plugin_->text("waiting");
      case Why::FAILED: return plugin_->text("wrong_sensor");
      case Why::ALL_CLOSED: return plugin_->text("none");
      default: return "";
    }
  }

  // What the second line says: closed or without prices when that matters, else the address when asked.
  std::string second_line(const Station &s, bool &alert) const {
    alert = false;
    if (status_ && s.state == Station::CLOSED) {
      alert = true;
      return plugin_->text("closed");
    }
    if (status_ && s.state == Station::NO_PRICES) return plugin_->text("no_prices");
    return address_ ? s.where : "";
  }

  // ---- One column ----------------------------------------------------------------------------------------------

  void make_single(lv_obj_t *parent) {
    const int line = ui::line_height(Font::BODY);
    arrow_w_ = ui::line_height(Font::ICON_SMALL);
    name_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(name_, width_ - arrow_w_ - ui::px(4), line);
    lv_obj_set_pos(name_, 0, 0);
    // How the last price changed, top right: up in red, down in green.
    arrow_ = ui::label(parent, Font::ICON_SMALL, theme::MUTED);
    lv_obj_set_size(arrow_, arrow_w_, arrow_w_);
    lv_obj_set_pos(arrow_, width_ - arrow_w_, (line - arrow_w_) / 2);
    big_ = ui::label(parent, Font::VALUE, theme::INK);
    small_ = ui::label(parent, Font::TITLE, theme::INK);
    foot_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(foot_, width_, line);
    lv_obj_set_pos(foot_, 0, height_ - line);
    make_fuel_rows(parent, line);
  }

  // With two or three fuels chosen and room for them, a row per fuel under the name: its name at the left, its price
  // at the right with its tenth and its arrow, in the largest face all rows fit in.
  void make_fuel_rows(lv_obj_t *parent, int top) {
    multi_ = false;
    if (fuel_count_ < 2) return;
    int label_w = 0;
    for (int k = 0; k < fuel_count_; ++k)
      label_w = std::max(label_w, ui::text_width(plugin_->text(FUEL_KEYS[fuels_[k]]), Font::BODY));
    const int room = height_ - top;
    for (Font f : {Font::HEADLINE, Font::TITLE, Font::BODY_LARGE}) {
      const int price_w = ui::text_width(std::string("8") + decimal_ + "88", f) + ui::text_width("8", Font::BODY);
      if (fuel_count_ * ui::line_height(f) <= room && label_w + ui::px(6) + price_w + arrow_w_ <= width_) {
        row_face_ = f;
        multi_ = true;
        break;
      }
    }
    if (!multi_) return;
    const int fh = ui::line_height(row_face_), bh = ui::line_height(Font::BODY), step = room / fuel_count_;
    for (int k = 0; k < fuel_count_; ++k) {
      FuelRow &r = fuel_rows_[k];
      const int y = top + k * step + (step - fh) / 2;
      r.label = ui::label(parent, Font::BODY, theme::MUTED);
      lv_obj_set_size(r.label, label_w + ui::px(2), bh);
      lv_obj_set_pos(r.label, 0, y + (fh - bh) / 2);
      r.price = ui::label(parent, row_face_, theme::INK);
      lv_obj_set_style_text_align(r.price, LV_TEXT_ALIGN_RIGHT, 0);
      r.tenth = ui::label(parent, Font::BODY, theme::INK);
      r.arrow = ui::label(parent, Font::ICON_SMALL, theme::MUTED);
      const int tw = ui::text_width("8", Font::BODY) + ui::px(2);
      const int price_x = label_w + ui::px(6);
      lv_obj_set_size(r.price, width_ - arrow_w_ - tw - price_x, fh);
      lv_obj_set_pos(r.price, price_x, y);
      lv_obj_set_size(r.tenth, tw, bh);
      lv_obj_set_pos(r.tenth, width_ - arrow_w_ - tw, y);
      lv_obj_set_size(r.arrow, arrow_w_, arrow_w_);
      lv_obj_set_pos(r.arrow, width_ - arrow_w_, y + (fh - arrow_w_) / 2);
    }
  }

  // The arrow of a price's last change, or nothing.
  void paint_arrow(lv_obj_t *arrow, const Station &s, int f) const {
    const int t = changes_ && s.state != Station::CLOSED ? s.trend[f] : 0;
    ui::set_text(arrow, t > 0 ? ui::icon(0xF005D) : t < 0 ? ui::icon(0xF0045) : "");
    ui::set_tone(arrow, t > 0 ? Tone::ALERT : t < 0 ? Tone::GOOD : Tone::NORMAL, theme::MUTED);
  }

  void paint_single(const Station **shown, int count, Why why) {
    const int line = ui::line_height(Font::BODY);
    if (why != Why::NONE || count == 0) {
      ui::set_text(name_, "");
      ui::set_text(small_, "");
      ui::set_text(foot_, "");
      ui::set_text(arrow_, "");
      clear_fuel_rows();
      place_big(why_text(why), "", Font::BODY_LARGE, theme::MUTED, line, height_);
      return;
    }
    const Station &s = *shown[0];
    if (multi_) {
      // The station's name, closed or without prices beside it, and a row per fuel.
      bool alert = false;
      const std::string second = status_ && s.state != Station::OPEN ? second_line(s, alert) : "";
      ui::set_text(name_, second.empty() ? std::string(s.title) : std::string(s.title) + " \u00B7 " + second);
      ui::set_tone(name_, alert ? Tone::ALERT : Tone::NORMAL, theme::MUTED);
      ui::set_text(arrow_, "");
      ui::set_text(foot_, "");
      ui::set_text(big_, "");
      ui::set_text(small_, "");
      for (int k = 0; k < fuel_count_; ++k) {
        FuelRow &r = fuel_rows_[k];
        const int fk = fuels_[k];
        std::string big, tenth;
        price_parts(s.price[fk], big, tenth);
        ui::set_text(r.label, plugin_->text(FUEL_KEYS[fk]));
        ui::set_color(r.label, theme::MUTED);
        ui::set_text(r.price, big);
        ui::set_text(r.tenth, tenth);
        const theme::Role normal = sort_ < 0 || sort_ == fk ? theme::INK : theme::INK_SOFT;
        tone_price(r.price, s, fk, normal);
        tone_price(r.tenth, s, fk, normal);
        paint_arrow(r.arrow, s, fk);
      }
      return;
    }
    const int f = main_fuel();
    paint_arrow(arrow_, s, f);
    ui::set_text(name_, s.title);
    bool alert = false;
    std::string foot = plugin_->text(FUEL_KEYS[f]);
    const std::string second = status_ && s.state != Station::OPEN ? second_line(s, alert) : "";
    if (!second.empty()) foot += " · " + second;
    ui::set_text(foot_, foot);
    ui::set_tone(foot_, alert ? Tone::ALERT : Tone::NORMAL, theme::MUTED);

    std::string big, tenth;
    price_parts(s.price[f], big, tenth);
    // The face is chosen for the shape of a price, not its digits, so it never jumps a size when a price changes.
    Font face = Font::BODY_LARGE;
    for (Font candidate : {Font::VALUE, Font::HEADLINE, Font::TITLE}) {
      const int w = ui::text_width(std::string("8") + decimal_ + "88", candidate) + ui::text_width("8", Font::TITLE);
      if (w <= width_ && ui::line_height(candidate) <= height_ - 2 * line) {
        face = candidate;
        break;
      }
    }
    place_big(big, tenth, face, theme::INK, line, height_ - line);
    tone_price(big_, s, f, theme::INK);
    tone_price(small_, s, f, theme::INK);
  }

  void clear_fuel_rows() {
    if (!multi_) return;
    for (int k = 0; k < fuel_count_; ++k) {
      ui::set_text(fuel_rows_[k].label, "");
      ui::set_text(fuel_rows_[k].price, "");
      ui::set_text(fuel_rows_[k].tenth, "");
      ui::set_text(fuel_rows_[k].arrow, "");
    }
  }

  // The large price between the name and the foot, in the middle, with its tenth of a cent raised beside it.
  void place_big(const std::string &big, const std::string &tenth, Font face, theme::Role role, int top, int bottom) {
    ui::set_font(big_, face);
    ui::set_color(big_, role);
    ui::set_text(big_, big);
    ui::set_text(small_, tenth);
    const int h = ui::line_height(face);
    const int bw = ui::text_width(big, face), sw = tenth.empty() ? 0 : ui::text_width(tenth, Font::TITLE) + ui::px(2);
    const int x = std::max(0, (width_ - bw - sw) / 2);
    const int y = top + std::max(0, (bottom - top - h) / 2);
    lv_obj_set_size(big_, std::max(1, std::min(bw + 1, width_)), h);
    lv_obj_set_pos(big_, x, y);
    lv_obj_set_size(small_, std::max(1, sw), ui::line_height(Font::TITLE));
    lv_obj_set_pos(small_, std::min(width_ - std::max(1, sw), x + bw + ui::px(2)), y + ui::px(2));
  }

  // ---- A list --------------------------------------------------------------------------------------------------

  void make_list(lv_obj_t *parent) {
    const int title_h = ui::line_height(Font::TITLE), body_h = ui::line_height(Font::BODY);
    head_h_ = body_h + ui::px(2);
    gap_ = ui::px(ui::large() ? 8 : 5);
    // One price column: the widest price there can be, its small digit, and a gap before the next one.
    col_w_ = ui::text_width(std::string("8") + decimal_ + "88", Font::TITLE) + ui::text_width("8", Font::BODY) + ui::px(4);
    col_gap_ = ui::px(ui::large() ? 10 : 6);
    // As many fuels as leave the name at least half of the width.
    shown_fuels_ = fuel_count_;
    while (shown_fuels_ > 1 && shown_fuels_ * (col_w_ + col_gap_) > width_ / 2) --shown_fuels_;
    name_w_ = std::max(1, width_ - shown_fuels_ * (col_w_ + col_gap_));
    // A second line under each name when there is one to say and room for at least two rows with it.
    two_lines_ = (address_ || status_) && head_h_ + 2 * (title_h + body_h + gap_) <= height_ + gap_;
    row_h_ = title_h + (two_lines_ ? body_h : 0);
    row_count_ = std::max(1, std::min(MAX_ROWS, (height_ - head_h_ + gap_) / (row_h_ + gap_)));

    note_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(note_, name_w_, body_h);
    lv_obj_set_pos(note_, 0, 0);
    for (int k = 0; k < shown_fuels_; ++k) {
      head_[k] = ui::label(parent, Font::BODY, theme::MUTED);
      lv_obj_set_style_text_align(head_[k], LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_set_size(head_[k], col_w_, body_h);
      lv_obj_set_pos(head_[k], column_x(k), 0);
      ui::set_text(head_[k], plugin_->text(FUEL_KEYS[fuels_[k]]));
    }
    for (int i = 0; i < row_count_; ++i) {
      Row &r = rows_[i];
      const int y = head_h_ + i * (row_h_ + gap_);
      r.title = ui::label(parent, Font::TITLE, theme::INK);
      lv_obj_set_size(r.title, name_w_ - ui::px(4), title_h);
      lv_obj_set_pos(r.title, 0, y);
      if (two_lines_) {
        r.where = ui::label(parent, Font::BODY, theme::MUTED);
        lv_obj_set_size(r.where, name_w_ - ui::px(4), body_h);
        lv_obj_set_pos(r.where, 0, y + title_h);
      }
      for (int k = 0; k < shown_fuels_; ++k) {
        r.price[k] = ui::label(parent, Font::TITLE, theme::INK);
        r.tenth[k] = ui::label(parent, Font::BODY, theme::INK);
      }
    }
  }

  int column_x(int k) const { return name_w_ + col_gap_ + k * (col_w_ + col_gap_); }

  // One price, right-aligned in its column: "1.79" and the small "9" raised at its right.
  void place_price(Row &r, int k, int y, const std::string &big, const std::string &tenth) {
    const int tw = ui::text_width("8", Font::BODY);
    const int bw = std::min(col_w_ - tw, ui::text_width(big, Font::TITLE) + 1);
    const int right = column_x(k) + col_w_;
    ui::set_text(r.price[k], big);
    ui::set_text(r.tenth[k], tenth);
    lv_obj_set_size(r.price[k], std::max(1, bw), ui::line_height(Font::TITLE));
    lv_obj_set_pos(r.price[k], right - tw - bw, y);
    lv_obj_set_size(r.tenth[k], tw, ui::line_height(Font::BODY));
    lv_obj_set_pos(r.tenth[k], right - tw, y);
  }

  void paint_list(const Station **shown, int count, Why why) {
    ui::set_text(note_, "");
    for (int i = 0; i < row_count_; ++i) {
      Row &r = rows_[i];
      const int y = head_h_ + i * (row_h_ + gap_);
      const Station *s = why == Why::NONE && i < count ? shown[i] : nullptr;
      if (!s) {
        ui::set_text(r.title, i == 0 ? why_text(why) : "");
        ui::set_color(r.title, theme::MUTED);
        if (r.where) ui::set_text(r.where, "");
        for (int k = 0; k < shown_fuels_; ++k) place_price(r, k, y, "", "");
        continue;
      }
      ui::set_text(r.title, s->title);
      ui::set_color(r.title, s->state == Station::CLOSED ? theme::MUTED : theme::INK);
      if (r.where) {
        bool alert = false;
        ui::set_text(r.where, second_line(*s, alert));
        ui::set_tone(r.where, alert ? Tone::ALERT : Tone::NORMAL, theme::MUTED);
      }
      for (int k = 0; k < shown_fuels_; ++k) {
        const int f = fuels_[k];
        std::string big, tenth;
        price_parts(s->price[f], big, tenth);
        place_price(r, k, y, big, tenth);
        // The fuel it sorts by is the one to read first: the others are a step softer.
        const theme::Role normal = sort_ < 0 || sort_ == f ? theme::INK : theme::INK_SOFT;
        tone_price(r.price[k], *s, f, normal);
        tone_price(r.tenth[k], *s, f, normal);
      }
    }
  }

  struct FuelRow {
    lv_obj_t *label{}, *price{}, *tenth{}, *arrow{};
  };

  Tankerkoenig *plugin_;
  int width_ = 0, height_ = 0, tile_ = -1;
  char decimal_[4] = ".";

  Station stations_[MAX_STATIONS]{};
  int station_count_ = 0;
  int fuels_[FUELS]{};
  int fuel_count_ = 0;
  int sort_ = -1, cheapest_ = 0;
  bool hide_closed_ = false, address_ = true, status_ = true, changes_ = true;
  char front_[48]{};
  Why wait_ = Why::CHOOSE;

  // One column.
  lv_obj_t *name_{}, *big_{}, *small_{}, *foot_{}, *arrow_{};
  bool multi_ = false;
  int arrow_w_ = 0;
  Font row_face_ = Font::TITLE;
  FuelRow fuel_rows_[FUELS];
  // A list.
  bool list_ = false, two_lines_ = false;
  int head_h_ = 0, gap_ = 0, col_w_ = 0, col_gap_ = 0, name_w_ = 0, row_h_ = 0, row_count_ = 0, shown_fuels_ = 0;
  lv_obj_t *note_{}, *head_[FUELS]{};
  Row rows_[MAX_ROWS];
};

// ---- The card: every station with every fuel, its address and whether it is open ----

class StationsCard : public tessera::Card {
 public:
  explicit StationsCard(Tankerkoenig *plugin) : plugin_(plugin), list_(plugin) {}

  void open(const tessera::CardContext &c) override {
    JsonDocument options;
    options["sort"] = "none";
    tessera::TileContext t{};
    t.parent = c.parent;
    t.width = c.width;
    t.height = c.height;
    t.columns = 4;
    t.rows = 4;
    t.name = "";
    t.entity = "";
    t.tile = -1;
    t.options = options.as<JsonObjectConst>();
    list_.create(t);
    list_.load(plugin_->card_stations, plugin_->card_count);
    list_.on_tick(0);
  }
  void on_state(JsonObjectConst data) override { list_.on_state(data); }
  void on_tick(uint32_t epoch) override { list_.on_tick(epoch); }
  void on_theme() override { list_.on_theme(); }

 private:
  Tankerkoenig *plugin_;
  PricesTile list_;
};

// ---- The plugin ----

void Tankerkoenig::setup() {
  add_tile("prices", [this]() { return new PricesTile(this); });
  add_card("stations", [this]() { return new StationsCard(this); });
  add_bar_item("cheap", [this]() { return bar(); });
}

// The fuel the top bar watches: the select's choice Diesel, E10 or E5 as the index of FUEL_KEYS.
int Tankerkoenig::alert_fuel() const {
  static const int BY_CHOICE[FUELS] = {2, 1, 0};
  if (!alert_fuel_) return 2;
  const auto index = alert_fuel_->active_index();
  return index.has_value() && *index < FUELS ? BY_CHOICE[*index] : 2;
}

// Home Assistant hands a list attribute to a text sensor as Python writes it: "[2.339, 0, 2.349]" and
// "['PM', 'JET']". These read it back.
static int read_numbers(const std::string &text, float *out, int most) {
  int n = 0;
  const char *p = text.c_str();
  while (*p && n < most) {
    if ((*p >= '0' && *p <= '9') || *p == '.') {
      char *end = nullptr;
      out[n++] = strtof(p, &end);
      p = end && end > p ? end : p + 1;
    } else {
      ++p;
    }
  }
  return n;
}

static int read_texts(const std::string &text, std::string *out, int most) {
  int n = 0;
  for (size_t i = 0; i < text.size() && n < most; ++i) {
    const char q = text[i];
    if (q != '\'' && q != '"') continue;
    const size_t end = text.find(q, i + 1);
    if (end == std::string::npos) break;
    out[n++] = text.substr(i + 1, end - i - 1);
    i = end;
  }
  return n;
}

// "Diesel 1,759 · JET" in the top bar while the cheapest station's price of the chosen fuel is at the limit or under.
tessera::BarItem Tankerkoenig::bar() const {
  tessera::BarItem item;
  if (!alert_ || !alert_->state || !alert_price_ || std::isnan(alert_price_->state)) return item;
  const int f = alert_fuel();
  if (!prices_[f] || !prices_[f]->has_state()) return item;
  float prices[MAX_STATIONS];
  const int n = read_numbers(prices_[f]->state, prices, MAX_STATIONS);
  int best = -1;
  for (int i = 0; i < n; ++i)
    if (prices[i] > 0 && (best < 0 || prices[i] < prices[best])) best = i;
  if (best < 0 || prices[best] * 100.0f > alert_price_->state + 0.001f) return item;
  std::string names[MAX_STATIONS];
  const int count = names_ && names_->has_state() ? read_texts(names_->state, names, MAX_STATIONS) : 0;
  char price[16];
  const long milli = lroundf(prices[best] * 1000.0f);
  snprintf(price, sizeof price, "%ld%s%03ld", milli / 1000, text("decimal"), milli % 1000);
  item.shown = true;
  item.icon = 0xF010B;  // car
  item.tone = Tone::GOOD;
  std::string words = tessera::fill(text("bar_cheap"), "fuel", text(FUEL_KEYS[f]));
  words = tessera::fill(words.c_str(), "price", price);
  item.text = tessera::fill(words.c_str(), "name", best < count ? names[best] : std::string());
  return item;
}

bool Tankerkoenig::settings(tessera::SettingsPage &page) {
  page.icon = "\U000F010B";
  page.toggle(text("alert"), [this]() { return alert_ && alert_->state; },
              [this](bool on) { if (alert_) { if (on) alert_->turn_on(); else alert_->turn_off(); } });
  page.choice(text("alert_fuel"), {text("diesel"), text("e10"), text("e5")},
              [this]() {
                const auto index = alert_fuel_ ? alert_fuel_->active_index() : optional<size_t>();
                return index.has_value() ? static_cast<int>(*index) : 0;
              },
              [this](int index) {
                if (!alert_fuel_) return;
                auto call = alert_fuel_->make_call();
                call.set_index(index);
                call.perform();
              });
  page.number(text("alert_price"), 100, 300, 1, "ct",
              [this]() { return alert_price_ && !std::isnan(alert_price_->state) ? static_cast<int>(alert_price_->state) : 180; },
              [this](int value) {
                if (!alert_price_) return;
                auto call = alert_price_->make_call();
                call.set_value(value);
                call.perform();
              });
  return true;
}

}  // namespace esphome::tankerkoenig

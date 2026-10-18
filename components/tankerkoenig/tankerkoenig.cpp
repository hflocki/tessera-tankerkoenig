#include "tankerkoenig.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace esphome::tankerkoenig {

using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

// The three fuels Tankerkoenig reports, in the order of its answer's fields.
static constexpr int FUELS = 3;
static const char *const FUEL_KEYS[FUELS] = {"e5", "e10", "diesel"};

static constexpr int MAX_STATIONS = 10;  // the option's `multiple`
static constexpr int MAX_ROWS = 6;       // rows the list view makes at most (memory in the manifest)

static int fuel_index(const char *key) {
  for (int i = 0; i < FUELS; ++i)
    if (key && strcmp(key, FUEL_KEYS[i]) == 0) return i;
  return -1;
}

static void copy(char *dst, size_t size, const char *src) {
  snprintf(dst, size, "%s", src ? src : "");
}

// One station of the template sensor's lists, and its last prices.
struct Station {
  char title[48];   // the brand or the station's name
  char where[64];   // street and town
  enum State : uint8_t { UNKNOWN, OPEN, CLOSED, NO_PRICES } state;
  float price[FUELS];  // 0: not sold or not known
  int8_t trend[FUELS]; // +1 went up, -1 went down, 0 unchanged since the tile knew it
};

// Why the tile has nothing to show; NONE when it has.
enum class Why : uint8_t { NONE, CHOOSE, WAITING, FAILED, ALL_CLOSED };

// The tile. One column: the first station (the cheapest when sorted) with one price large. Two columns or more: a
// header with the fuels and a row per station, its name and address on the left and a price per fuel on the right.
// Home Assistant sends the sensor again at every change; the order, the filter and the colours are worked out again on every tick,
// which for ten stations costs nothing, and ui::set_* change a label only when it differs.
class PricesTile : public tessera::Tile {
 public:
  explicit PricesTile(const tessera::Plugin *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
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
    return n;
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
    name_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(name_, width_, ui::line_height(Font::BODY));
    lv_obj_set_pos(name_, 0, 0);
    big_ = ui::label(parent, Font::VALUE, theme::INK);
    small_ = ui::label(parent, Font::TITLE, theme::INK);
    foot_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(foot_, width_, ui::line_height(Font::BODY));
    lv_obj_set_pos(foot_, 0, height_ - ui::line_height(Font::BODY));
  }

  void paint_single(const Station **shown, int count, Why why) {
    const int line = ui::line_height(Font::BODY);
    if (why != Why::NONE || count == 0) {
      ui::set_text(name_, "");
      ui::set_text(small_, "");
      ui::set_text(foot_, "");
      place_big(why_text(why), "", Font::BODY_LARGE, theme::MUTED, line, height_);
      return;
    }
    const Station &s = *shown[0];
    const int f = main_fuel();
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

  const tessera::Plugin *plugin_;
  int width_ = 0, height_ = 0;
  char decimal_[4] = ".";

  Station stations_[MAX_STATIONS]{};
  int station_count_ = 0;
  int fuels_[FUELS]{};
  int fuel_count_ = 0;
  int sort_ = -1, cheapest_ = 0;
  bool hide_closed_ = false, address_ = true, status_ = true, changes_ = true;
  Why wait_ = Why::CHOOSE;

  // One column.
  lv_obj_t *name_{}, *big_{}, *small_{}, *foot_{};
  // A list.
  bool list_ = false, two_lines_ = false;
  int head_h_ = 0, gap_ = 0, col_w_ = 0, col_gap_ = 0, name_w_ = 0, row_h_ = 0, row_count_ = 0, shown_fuels_ = 0;
  lv_obj_t *note_{}, *head_[FUELS]{};
  Row rows_[MAX_ROWS];
};

void Tankerkoenig::setup() {
  add_tile("prices", [this]() { return new PricesTile(this); });
}

}  // namespace esphome::tankerkoenig

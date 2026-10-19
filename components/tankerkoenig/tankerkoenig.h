#pragma once
#include <cstdint>
#include <string>
#include "esphome/core/component.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::tankerkoenig {

// The three fuels Tankerkoenig reports, in the order of its answer's fields.
static constexpr int FUELS = 3;
inline const char *const FUEL_KEYS[FUELS] = {"e5", "e10", "diesel"};

static constexpr int MAX_STATIONS = 10;  // the option's `multiple`
static constexpr int MAX_ROWS = 6;       // rows the list view makes at most (memory in the manifest)

// One station of the template sensor's lists, and its last prices.
struct Station {
  char title[48];   // the brand or the station's name
  char where[64];   // street and town
  enum State : uint8_t { UNKNOWN, OPEN, CLOSED, NO_PRICES } state;
  float price[FUELS];  // 0: not sold or not known
  int8_t trend[FUELS]; // +1 went up, -1 went down, 0 unchanged since the tile knew it
};

class PricesTile;

// The plugin: a tile ("prices"), a card with every station and fuel ("stations"), an item for the top bar ("cheap")
// and three rows on the screen's settings page. The top bar reads the template sensor itself (plugin.yaml), the tile
// and the card get it from the app.
class Tankerkoenig : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  bool settings(tessera::SettingsPage &page) override;

  void set_names(text_sensor::TextSensor *s) { names_ = s; }
  void set_prices(int fuel, text_sensor::TextSensor *s) { if (fuel >= 0 && fuel < FUELS) prices_[fuel] = s; }
  void set_alert(switch_::Switch *s) { alert_ = s; }
  void set_alert_fuel(select::Select *s) { alert_fuel_ = s; }
  void set_alert_price(number::Number *n) { alert_price_ = n; }

  // The stations of the tile that opens the card, for the card's first look.
  Station card_stations[MAX_STATIONS]{};
  int card_count = 0;

 protected:
  tessera::BarItem bar() const;
  int alert_fuel() const;
  text_sensor::TextSensor *names_{}, *prices_[FUELS]{};
  switch_::Switch *alert_{};
  select::Select *alert_fuel_{};
  number::Number *alert_price_{};
};

}  // namespace esphome::tankerkoenig

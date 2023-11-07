/*
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2023 Kyle Reed
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "ui.hpp"
#include "ui_receiver.hpp"
#include "ui_geomap.hpp"

#include "adsb.hpp"
#include "app_settings.hpp"
#include "crc.hpp"
#include "database.hpp"
#include "file.hpp"
#include "log_file.hpp"
#include "message.hpp"
#include "radio_state.hpp"
#include "recent_entries.hpp"
#include "string_format.hpp"

using namespace adsb;

namespace ui {
#define AIRCRAFT_ID_L 1  // aircraft ID message type (lowest type id)
#define AIRCRAFT_ID_H 4  // aircraft ID message type (highest type id)

#define SURFACE_POS_L 5  // surface position (lowest type id)
#define SURFACE_POS_H 8  // surface position (highest type id)

#define AIRBORNE_POS_BARO_L 9   // airborne position (lowest type id)
#define AIRBORNE_POS_BARO_H 18  // airborne position (highest type id)

#define AIRBORNE_VEL 19  // airborne velocities

#define AIRBORNE_POS_GPS_L 20  // airborne position (lowest type id)
#define AIRBORNE_POS_GPS_H 22  // airborne position (highest type id)

#define RESERVED_L 23  // reserved for other uses
#define RESERVED_H 31  // reserved for other uses

#define VEL_GND_SUBSONIC 1
#define VEL_GND_SUPERSONIC 2
#define VEL_AIR_SUBSONIC 3
#define VEL_AIR_SUPERSONIC 4

#define O_E_FRAME_TIMEOUT 20  // timeout between odd and even frames
#define MARKER_UPDATE_SECONDS 5

/* Thresholds (in seconds) that define the transition between ages. */
struct ADSBAgeLimit {
    static constexpr int Current = 10;
    static constexpr int Recent = 30;
    static constexpr int Expired = 300;
};

/* Age states used for sorting and drawing recent entries. */
enum class ADSBAgeState : uint8_t {
    Invalid,
    Current,
    Recent,
    Old,
    Expired,
};

struct AircraftRecentEntry {
    using Key = uint32_t;

    static constexpr Key invalid_key = 0xffffffff;

    uint32_t ICAO_address{};
    uint16_t hits{0};

    ADSBAgeState state{ADSBAgeState::Invalid};
    uint32_t age{0};  // In seconds
    uint32_t amp{0};
    adsb_pos pos{false, 0, 0, 0};
    adsb_vel velo{false, 0, 999, 0};
    ADSBFrame frame_pos_even{};
    ADSBFrame frame_pos_odd{};

    std::string icao_str{"      "};
    std::string callsign{"        "};
    std::string info_string{""};

    AircraftRecentEntry(const uint32_t ICAO_address)
        : ICAO_address{ICAO_address} {
        this->icao_str = to_string_hex(ICAO_address, 6);
    }

    /* RecentEntries helpers expect a "key" on every item. */
    Key key() const {
        return ICAO_address;
    }

    void set_callsign(std::string new_callsign) {
        callsign = std::move(new_callsign);
    }

    void inc_hit() {
        hits++;
    }

    void set_frame_pos(ADSBFrame& frame, uint32_t parity) {
        if (!parity)
            frame_pos_even = frame;
        else
            frame_pos_odd = frame;

        if (!frame_pos_even.empty() && !frame_pos_odd.empty()) {
            if (abs(frame_pos_even.get_rx_timestamp() - frame_pos_odd.get_rx_timestamp()) < O_E_FRAME_TIMEOUT)
                pos = decode_frame_pos(frame_pos_even, frame_pos_odd);
        }
    }

    void set_frame_velo(ADSBFrame& frame) {
        velo = decode_frame_velo(frame);
    }

    void set_info_string(std::string new_info_string) {
        info_string = std::move(new_info_string);
    }

    void reset_age() {
        age = 0;
    }

    void inc_age(int delta) {
        age += delta;

        if (age < ADSBAgeLimit::Current)
            state = pos.valid ? ADSBAgeState::Invalid
                              : ADSBAgeState::Current;

        else if (age < ADSBAgeLimit::Recent)
            state = ADSBAgeState::Recent;

        else if (age < ADSBAgeLimit::Expired)
            state = ADSBAgeState::Old;

        else
            state = ADSBAgeState::Expired;
    }
};

// NB: uses std::list underneath so assuming refs are NOT invalidated.
using AircraftRecentEntries = RecentEntries<AircraftRecentEntry>;

struct ADSBLogEntry {
    std::string raw_data{};
    std::string icao{};
    std::string callsign{};
    adsb_pos pos{};
    adsb_vel vel{};
    uint8_t vel_type{};
};

class ADSBLogger {
   public:
    Optional<File::Error> append(const std::filesystem::path& filename) {
        return log_file.append(filename);
    }
    void log(const ADSBLogEntry& log_entry);

   private:
    LogFile log_file{};
};

class ADSBRxAircraftDetailsView : public View {
   public:
    ADSBRxAircraftDetailsView(
        NavigationView&,
        const AircraftRecentEntry& entry,
        std::function<void(void)> on_close);
    ~ADSBRxAircraftDetailsView();

    ADSBRxAircraftDetailsView(const ADSBRxAircraftDetailsView&) = delete;
    ADSBRxAircraftDetailsView(ADSBRxAircraftDetailsView&&) = delete;
    ADSBRxAircraftDetailsView& operator=(const ADSBRxAircraftDetailsView&) = delete;
    ADSBRxAircraftDetailsView& operator=(ADSBRxAircraftDetailsView&&) = delete;

    void focus() override;

    void update(const AircraftRecentEntry& entry);

    std::string title() const override { return "AC Details"; };

    AircraftRecentEntry get_current_entry() { return entry_copy; }

    std::database::AircraftDBRecord aircraft_record = {};

   private:
    AircraftRecentEntry entry_copy{0};  // Why value?
    std::function<void(void)> on_close_{};
    bool send_updates{false};
    std::database db = {};
    std::string icao_code = "";
    int return_code = 0;

    Labels labels{
        {{0 * 8, 1 * 16}, "ICAO:", Color::light_grey()},
        {{0 * 8, 2 * 16}, "Registration:", Color::light_grey()},
        {{0 * 8, 3 * 16}, "Manufacturer:", Color::light_grey()},
        {{0 * 8, 5 * 16}, "Model:", Color::light_grey()},
        {{0 * 8, 7 * 16}, "Type:", Color::light_grey()},
        {{0 * 8, 8 * 16}, "Number of engines:", Color::light_grey()},
        {{0 * 8, 9 * 16}, "Engine type:", Color::light_grey()},
        {{0 * 8, 11 * 16}, "Owner:", Color::light_grey()},
        {{0 * 8, 13 * 16}, "Operator:", Color::light_grey()}};

    Text text_icao_address{
        {5 * 8, 1 * 16, 6 * 8, 16},
        "-"};

    Text text_registration{
        {13 * 8, 2 * 16, 8 * 8, 16},
        "-"};

    Text text_manufacturer{
        {0 * 8, 4 * 16, 19 * 8, 16},
        "-"};

    Text text_model{
        {0 * 8, 6 * 16, 30 * 8, 16},
        "-"};

    Text text_type{
        {5 * 8, 7 * 16, 22 * 8, 16},
        "-"};

    Text text_number_of_engines{
        {18 * 8, 8 * 16, 30 * 8, 16},
        "-"};

    Text text_engine_type{
        {0 * 8, 10 * 16, 30 * 8, 16},
        "-"};

    Text text_owner{
        {0 * 8, 12 * 16, 30 * 8, 16},
        "-"};

    Text text_operator{
        {0 * 8, 14 * 16, 30 * 8, 16},
        "-"};

    Button button_close{
        {9 * 8, 16 * 16, 12 * 8, 3 * 16},
        "Back"};
};

class ADSBRxDetailsView : public View {
   public:
    ADSBRxDetailsView(NavigationView&, const AircraftRecentEntry& entry, const std::function<void(void)> on_close);
    ~ADSBRxDetailsView();

    ADSBRxDetailsView(const ADSBRxDetailsView&) = delete;
    ADSBRxDetailsView(ADSBRxDetailsView&&) = delete;
    ADSBRxDetailsView& operator=(const ADSBRxDetailsView&) = delete;
    ADSBRxDetailsView& operator=(ADSBRxDetailsView&&) = delete;

    void focus() override;

    void update(const AircraftRecentEntry& entry);

    std::string title() const override { return "Details"; };

    AircraftRecentEntry get_current_entry() { return entry_copy; }

    std::database::AirlinesDBRecord airline_record = {};

    GeoMapView* geomap_view{nullptr};

   private:
    AircraftRecentEntry entry_copy{0};
    std::function<void(void)> on_close_{};
    ADSBRxAircraftDetailsView* aircraft_details_view{nullptr};
    bool send_updates{false};
    std::database db = {};
    std::string airline_code = "";
    int return_code = 0;

    Labels labels{
        {{0 * 8, 1 * 16}, "ICAO:", Color::light_grey()},
        {{13 * 8, 1 * 16}, "Callsign:", Color::light_grey()},
        {{0 * 8, 2 * 16}, "Last seen:", Color::light_grey()},
        {{0 * 8, 3 * 16}, "Airline:", Color::light_grey()},
        {{0 * 8, 5 * 16}, "Country:", Color::light_grey()},
        {{0 * 8, 13 * 16}, "Even position frame:", Color::light_grey()},
        {{0 * 8, 15 * 16}, "Odd position frame:", Color::light_grey()}};

    Text text_icao_address{
        {5 * 8, 1 * 16, 6 * 8, 16},
        "-"};

    Text text_callsign{
        {22 * 8, 1 * 16, 8 * 8, 16},
        "-"};

    Text text_last_seen{
        {11 * 8, 2 * 16, 19 * 8, 16},
        "-"};

    Text text_airline{
        {0 * 8, 4 * 16, 30 * 8, 16},
        "-"};

    Text text_country{
        {8 * 8, 5 * 16, 22 * 8, 16},
        "-"};

    Text text_infos{
        {0 * 8, 6 * 16, 30 * 8, 16},
        "-"};

    Text text_info2{
        {0 * 8, 7 * 16, 30 * 8, 16},
        "-"};

    Text text_frame_pos_even{
        {0 * 8, 14 * 16, 30 * 8, 16},
        "-"};
    Text text_frame_pos_odd{
        {0 * 8, 16 * 16, 30 * 8, 16},
        "-"};

    Button button_aircraft_details{
        {2 * 8, 9 * 16, 12 * 8, 3 * 16},
        "A/C details"};

    Button button_see_map{
        {16 * 8, 9 * 16, 12 * 8, 3 * 16},
        "See on map"};
};

/* Main ADSB application view and message dispatch. */
class ADSBRxView : public View {
   public:
    ADSBRxView(NavigationView& nav);
    ~ADSBRxView();

    ADSBRxView(const ADSBRxView&) = delete;
    ADSBRxView(ADSBRxView&&) = delete;
    ADSBRxView& operator=(const ADSBRxView&) = delete;
    ADSBRxView& operator=(ADSBRxView&&) = delete;

    void focus() override;
    std::string title() const override { return "ADS-B RX"; };

   private:
    RxRadioState radio_state_{
        1'090'000'000 /* frequency */,
        2'500'000 /* bandwidth */,
        2'000'000 /* sampling rate */,
        ReceiverModel::Mode::SpectrumAnalysis};  // Why spectrum?
    app_settings::SettingsManager settings_{
        "rx_adsb", app_settings::Mode::RX};

    std::unique_ptr<ADSBLogger> logger{};

    /* Event Handlers */
    void on_frame(const ADSBFrameMessage* message);
    void on_tick_second();
    void update_recent_entries();
    void update_details_and_map(int age_delta);
    uint8_t tick_count = 0;

    /* Max number of entries that can be updated in a single pass.
     * 16 is one screen of recent entries. */
    static constexpr uint8_t max_update_entries = 16;

    SignalToken signal_token_tick_second{};
    int ticksSinceMarkerRefresh{MARKER_UPDATE_SECONDS - 1};

    /* Recent Entries */
    const RecentEntriesColumns columns{{{"ICAO/Call", 9},
                                        {"Lvl", 3},
                                        {"Spd", 3},
                                        {"Amp", 3},
                                        {"Hit", 3},
                                        {"Age", 4}}};
    AircraftRecentEntries recent{};
    RecentEntriesView<AircraftRecentEntries> recent_entries_view{columns, recent};

    /* Entry Management */
    AircraftRecentEntry& find_or_create_entry(uint32_t ICAO_address);
    void sort_entries_by_state();
    void remove_expired_entries();

    ADSBRxDetailsView* details_view{nullptr};  // Why is this here?
    uint32_t detailed_entry_key{0};            // Key?
    bool send_updates{false};                  // Why is this here?

    Labels labels{
        {{0 * 8, 0 * 8}, "LNA:   VGA:   AMP:", Color::light_grey()}};

    LNAGainField field_lna{
        {4 * 8, 0 * 16}};

    VGAGainField field_vga{
        {11 * 8, 0 * 16}};

    RFAmpField field_rf_amp{
        {18 * 8, 0 * 16}};

    RSSI rssi{
        {20 * 8, 4, 10 * 7, 8},
    };

    ActivityDot status_frame{
        {screen_width - 3, 5, 2, 2},
        Color::light_grey(),
    };

    ActivityDot status_good_frame{
        {screen_width - 3, 9, 2, 2},
        Color::white(),
    };

    MessageHandlerRegistration message_handler_frame{
        Message::ID::ADSBFrame,
        [this](Message* const p) {
            const auto message = static_cast<const ADSBFrameMessage*>(p);
            this->on_frame(message);
        }};
};

} /* namespace ui */

#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <unordered_map>

namespace axiom::core {

// Fast local-time conversion for directory-sized batches. The CRT and the
// timezone conversion APIs refresh/lock timezone state on every call; that is
// surprisingly expensive when an archive contains tens of thousands of files.
// Windows exposes historical rules by year, so cache those rules and perform
// the remaining calendar arithmetic locally.
class LocalTimeConverter {
public:
    std::int64_t local_to_unix(const SYSTEMTIME& local) {
        const std::int64_t naive = naive_unix_seconds(local);
        const YearRules& rules = rules_for(local.wYear);
        const LONG bias = is_daylight_local(naive, rules)
            ? rules.daylight_bias
            : rules.standard_bias;
        return naive + static_cast<std::int64_t>(bias) * 60;
    }

    bool unix_to_local(std::int64_t seconds, SYSTEMTIME& local) {
        SYSTEMTIME utc{};
        if (!unix_to_system_time(seconds, utc)) return false;
        const YearRules& rules = rules_for(utc.wYear);
        const LONG bias = is_daylight_utc(seconds, rules)
            ? rules.daylight_bias
            : rules.standard_bias;
        return unix_to_system_time(seconds - static_cast<std::int64_t>(bias) * 60, local);
    }

private:
    struct YearRules {
        LONG standard_bias = 0;
        LONG daylight_bias = 0;
        bool has_daylight = false;
        std::int64_t daylight_start_local = 0;
        std::int64_t standard_start_local = 0;
        std::int64_t daylight_start_utc = 0;
        std::int64_t standard_start_utc = 0;
    };

    static constexpr std::int64_t days_from_civil(int year, unsigned month,
                                                   unsigned day) noexcept {
        year -= month <= 2;
        const int era = (year >= 0 ? year : year - 399) / 400;
        const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
        const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
        const unsigned day_of_year = (153u * adjusted_month + 2u) / 5u + day - 1u;
        const unsigned day_of_era = year_of_era * 365u + year_of_era / 4u -
                                    year_of_era / 100u + day_of_year;
        return static_cast<std::int64_t>(era) * 146097 +
               static_cast<std::int64_t>(day_of_era) - 719468;
    }

    static bool leap_year(unsigned year) noexcept {
        return year % 4u == 0u && (year % 100u != 0u || year % 400u == 0u);
    }

    static unsigned days_in_month(unsigned year, unsigned month) noexcept {
        constexpr unsigned lengths[] = {31, 28, 31, 30, 31, 30,
                                        31, 31, 30, 31, 30, 31};
        if (month == 2) return leap_year(year) ? 29u : 28u;
        return month >= 1 && month <= 12 ? lengths[month - 1] : 0u;
    }

    static unsigned weekday(unsigned year, unsigned month, unsigned day) noexcept {
        // 1970-01-01 was Thursday; SYSTEMTIME uses Sunday == 0.
        std::int64_t value = (days_from_civil(static_cast<int>(year), month, day) + 4) % 7;
        if (value < 0) value += 7;
        return static_cast<unsigned>(value);
    }

    static unsigned transition_day(unsigned year, const SYSTEMTIME& rule) noexcept {
        if (rule.wYear != 0) return rule.wDay;
        const unsigned first_weekday = weekday(year, rule.wMonth, 1);
        unsigned day = 1u + (static_cast<unsigned>(rule.wDayOfWeek) + 7u - first_weekday) % 7u;
        day += 7u * (static_cast<unsigned>(rule.wDay) - 1u);
        const unsigned last = days_in_month(year, rule.wMonth);
        if (rule.wDay == 5 && day > last) day -= 7u;
        return day;
    }

    static std::int64_t naive_unix_seconds(const SYSTEMTIME& value) noexcept {
        return days_from_civil(value.wYear, value.wMonth, value.wDay) * 86400 +
               static_cast<std::int64_t>(value.wHour) * 3600 +
               static_cast<std::int64_t>(value.wMinute) * 60 + value.wSecond;
    }

    static SYSTEMTIME resolved_transition(unsigned year, const SYSTEMTIME& rule) noexcept {
        SYSTEMTIME result = rule;
        result.wYear = static_cast<WORD>(year);
        result.wDay = static_cast<WORD>(transition_day(year, rule));
        return result;
    }

    static bool in_interval(std::int64_t value, std::int64_t start,
                            std::int64_t end) noexcept {
        if (start == end) return false;
        return start < end ? value >= start && value < end
                           : value >= start || value < end;
    }

    static bool is_daylight_local(std::int64_t value, const YearRules& rules) noexcept {
        return rules.has_daylight &&
               in_interval(value, rules.daylight_start_local, rules.standard_start_local);
    }

    static bool is_daylight_utc(std::int64_t value, const YearRules& rules) noexcept {
        return rules.has_daylight &&
               in_interval(value, rules.daylight_start_utc, rules.standard_start_utc);
    }

    static bool unix_to_system_time(std::int64_t seconds, SYSTEMTIME& result) noexcept {
        constexpr std::int64_t epoch_difference = 11644473600ll;
        if (seconds < -epoch_difference) return false;
        const std::uint64_t ticks =
            static_cast<std::uint64_t>(seconds + epoch_difference) * 10000000ull;
        FILETIME file_time{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
        return FileTimeToSystemTime(&file_time, &result) != FALSE;
    }

    YearRules make_rules(unsigned year) const {
        DYNAMIC_TIME_ZONE_INFORMATION dynamic{};
        GetDynamicTimeZoneInformation(&dynamic);
        TIME_ZONE_INFORMATION zone{};
        if (!GetTimeZoneInformationForYear(static_cast<USHORT>(year), &dynamic, &zone)) {
            zone.Bias = dynamic.Bias;
            zone.StandardBias = dynamic.StandardBias;
            zone.DaylightBias = dynamic.DaylightBias;
            zone.StandardDate = dynamic.StandardDate;
            zone.DaylightDate = dynamic.DaylightDate;
        }

        YearRules rules;
        rules.standard_bias = zone.Bias + zone.StandardBias;
        rules.daylight_bias = zone.Bias + zone.DaylightBias;
        rules.has_daylight = zone.DaylightDate.wMonth != 0 &&
                             zone.StandardDate.wMonth != 0 &&
                             rules.standard_bias != rules.daylight_bias;
        if (!rules.has_daylight) return rules;

        const SYSTEMTIME daylight = resolved_transition(year, zone.DaylightDate);
        const SYSTEMTIME standard = resolved_transition(year, zone.StandardDate);
        rules.daylight_start_local = naive_unix_seconds(daylight);
        rules.standard_start_local = naive_unix_seconds(standard);
        // The clock is still on standard time at the daylight transition and
        // still on daylight time at the standard transition.
        rules.daylight_start_utc = rules.daylight_start_local +
                                   static_cast<std::int64_t>(rules.standard_bias) * 60;
        rules.standard_start_utc = rules.standard_start_local +
                                   static_cast<std::int64_t>(rules.daylight_bias) * 60;
        return rules;
    }

    const YearRules& rules_for(unsigned year) {
        auto found = years_.find(year);
        if (found == years_.end()) found = years_.emplace(year, make_rules(year)).first;
        return found->second;
    }

    std::unordered_map<unsigned, YearRules> years_;
};

}  // namespace axiom::core
#endif

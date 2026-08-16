#include "core/datetime_names.h"

#include <cctype>

namespace vxcore {

// NOTE: every non-ASCII entry below is written as explicit UTF-8 byte escapes.
// The production `vxcore` target is NOT compiled with MSVC's /utf-8 flag (that
// flag is applied directory-wide only under libs/vxcore/tests), so a raw CJK
// literal here would be re-encoded using the active ANSI code page in the DLL
// while the direct-compiled test copy compiled correctly. The escapes remove
// the source-encoding dependence entirely. Do NOT "clean them up" into raw
// literals, and keep the comments in this file ASCII-only.
namespace {

// ---------------------------------------------------------------------------
// English
// ---------------------------------------------------------------------------
const char *const kEnShortMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char *const kEnLongMonths[12] = {"January",   "February", "March",    "April",
                                       "May",       "June",     "July",     "August",
                                       "September", "October",  "November", "December"};
const char *const kEnShortDays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char *const kEnLongDays[7] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                    "Thursday", "Friday", "Saturday"};

// ---------------------------------------------------------------------------
// Shared CJK building blocks (UTF-8 byte escapes).
// ---------------------------------------------------------------------------
#define VX_YUE "\xE6\x9C\x88"   // U+6708 month
#define VX_ZHOU "\xE5\x91\xA8"  // U+5468 week (zh abbreviated day prefix)
#define VX_XING "\xE6\x98\x9F"  // U+661F
#define VX_QI "\xE6\x9C\x9F"    // U+671F
#define VX_YAO "\xE6\x9B\x9C"   // U+66DC (ja day-of-week marker)

#define VX_RI "\xE6\x97\xA5"     // U+65E5 sun/day
#define VX_YUE_MOON "\xE6\x9C\x88"  // U+6708 moon (same glyph as month)
#define VX_HUO "\xE7\x81\xAB"    // U+706B fire
#define VX_SHUI "\xE6\xB0\xB4"   // U+6C34 water
#define VX_MU "\xE6\x9C\xA8"     // U+6728 wood
#define VX_JIN "\xE9\x87\x91"    // U+91D1 metal
#define VX_TU "\xE5\x9C\x9F"     // U+571F earth

#define VX_YI "\xE4\xB8\x80"    // U+4E00 one
#define VX_ER "\xE4\xBA\x8C"    // U+4E8C two
#define VX_SAN "\xE4\xB8\x89"   // U+4E09 three
#define VX_SI "\xE5\x9B\x9B"    // U+56DB four
#define VX_WU "\xE4\xBA\x94"    // U+4E94 five
#define VX_LIU "\xE5\x85\xAD"   // U+516D six
#define VX_QId "\xE4\xB8\x83"   // U+4E03 seven
#define VX_BA "\xE5\x85\xAB"    // U+516B eight
#define VX_JIU "\xE4\xB9\x9D"   // U+4E5D nine
#define VX_SHI "\xE5\x8D\x81"   // U+5341 ten

// ---------------------------------------------------------------------------
// Simplified Chinese (zh_CN)
// ---------------------------------------------------------------------------
const char *const kZhShortMonths[12] = {
    "1" VX_YUE, "2" VX_YUE,  "3" VX_YUE,  "4" VX_YUE,  "5" VX_YUE,  "6" VX_YUE,
    "7" VX_YUE, "8" VX_YUE,  "9" VX_YUE,  "10" VX_YUE, "11" VX_YUE, "12" VX_YUE};
const char *const kZhLongMonths[12] = {
    VX_YI VX_YUE,          VX_ER VX_YUE,         VX_SAN VX_YUE,        VX_SI VX_YUE,
    VX_WU VX_YUE,          VX_LIU VX_YUE,        VX_QId VX_YUE,        VX_BA VX_YUE,
    VX_JIU VX_YUE,         VX_SHI VX_YUE,        VX_SHI VX_YI VX_YUE,  VX_SHI VX_ER VX_YUE};
const char *const kZhShortDays[7] = {VX_ZHOU VX_RI,  VX_ZHOU VX_YI,  VX_ZHOU VX_ER,
                                     VX_ZHOU VX_SAN, VX_ZHOU VX_SI,  VX_ZHOU VX_WU,
                                     VX_ZHOU VX_LIU};
const char *const kZhLongDays[7] = {
    VX_XING VX_QI VX_RI,  VX_XING VX_QI VX_YI, VX_XING VX_QI VX_ER, VX_XING VX_QI VX_SAN,
    VX_XING VX_QI VX_SI,  VX_XING VX_QI VX_WU, VX_XING VX_QI VX_LIU};

// ---------------------------------------------------------------------------
// Japanese (ja)
// ---------------------------------------------------------------------------
const char *const kJaShortMonths[12] = {
    "1" VX_YUE, "2" VX_YUE, "3" VX_YUE,  "4" VX_YUE,  "5" VX_YUE,  "6" VX_YUE,
    "7" VX_YUE, "8" VX_YUE, "9" VX_YUE,  "10" VX_YUE, "11" VX_YUE, "12" VX_YUE};
const char *const *const kJaLongMonths = kJaShortMonths;
const char *const kJaShortDays[7] = {VX_RI,  VX_YUE_MOON, VX_HUO, VX_SHUI,
                                     VX_MU,  VX_JIN,      VX_TU};
const char *const kJaLongDays[7] = {
    VX_RI VX_YAO VX_RI,  VX_YUE_MOON VX_YAO VX_RI, VX_HUO VX_YAO VX_RI, VX_SHUI VX_YAO VX_RI,
    VX_MU VX_YAO VX_RI,  VX_JIN VX_YAO VX_RI,      VX_TU VX_YAO VX_RI};

std::string NormalizeLocale(const std::string &locale) {
  std::string s;
  s.reserve(locale.size());
  for (char c : locale) {
    if (c == '-') {
      s.push_back('_');
    } else {
      s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  }
  return s;
}

}  // namespace

std::string CanonicalizeLocale(const std::string &locale) {
  const std::string s = NormalizeLocale(locale);
  if (s.empty()) {
    return "en";
  }

  // Exact match on the canonical names first.
  if (s == "en") return "en";
  if (s == "zh_cn") return "zh_CN";
  if (s == "ja") return "ja";

  // Language subtag match.
  const std::string lang = s.substr(0, s.find('_'));
  if (lang == "zh") return "zh_CN";
  if (lang == "ja") return "ja";
  if (lang == "en") return "en";

  return "en";
}

const DateTimeNames &DateTimeNames::ForLocale(const std::string &locale) {
  static const DateTimeNames kEn("en", kEnShortMonths, kEnLongMonths, kEnShortDays, kEnLongDays);
  static const DateTimeNames kZh("zh_CN", kZhShortMonths, kZhLongMonths, kZhShortDays,
                                 kZhLongDays);
  static const DateTimeNames kJa("ja", kJaShortMonths, kJaLongMonths, kJaShortDays, kJaLongDays);

  const std::string canonical = CanonicalizeLocale(locale);
  if (canonical == "zh_CN") return kZh;
  if (canonical == "ja") return kJa;
  return kEn;
}

const char *DateTimeNames::ShortMonth(int tm_mon) const {
  if (tm_mon < 0 || tm_mon > 11) tm_mon = 0;
  return short_months_[tm_mon];
}

const char *DateTimeNames::LongMonth(int tm_mon) const {
  if (tm_mon < 0 || tm_mon > 11) tm_mon = 0;
  return long_months_[tm_mon];
}

const char *DateTimeNames::ShortDay(int tm_wday) const {
  if (tm_wday < 0 || tm_wday > 6) tm_wday = 0;
  return short_days_[tm_wday];
}

const char *DateTimeNames::LongDay(int tm_wday) const {
  if (tm_wday < 0 || tm_wday > 6) tm_wday = 0;
  return long_days_[tm_wday];
}

const char *DateTimeNames::CanonicalName() const { return canonical_name_; }

}  // namespace vxcore

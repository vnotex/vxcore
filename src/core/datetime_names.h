#ifndef VXCORE_DATETIME_NAMES_H
#define VXCORE_DATETIME_NAMES_H

#include <string>

namespace vxcore {

// UTF-8 month/day names for a supported locale. Immutable, statically allocated.
//
// The tables are locale-independent data compiled into the library: they never
// consult the C runtime locale (`setlocale`/`LC_TIME`) or `strftime`, so the
// bytes returned are always valid UTF-8 regardless of the host locale or the
// Windows ANSI code page.
class DateTimeNames {
 public:
  // |locale| accepts "en", "en_US", "zh_CN", "zh-CN", "ja_JP" (case-insensitive,
  // '-' normalized to '_'). Exact match, then language subtag, else English.
  // Never returns null. The returned object is statically allocated and
  // immutable; there is no global mutable state, so this is safe to call from
  // any thread.
  static const DateTimeNames &ForLocale(const std::string &locale);

  const char *ShortMonth(int tm_mon) const;  // 0..11, out of range -> January
  const char *LongMonth(int tm_mon) const;
  const char *ShortDay(int tm_wday) const;  // 0..6, 0 = Sunday, out of range -> Sunday
  const char *LongDay(int tm_wday) const;
  const char *CanonicalName() const;  // "en" | "zh_CN" | "ja"

 private:
  DateTimeNames(const char *canonical_name, const char *const *short_months,
                const char *const *long_months, const char *const *short_days,
                const char *const *long_days)
      : canonical_name_(canonical_name),
        short_months_(short_months),
        long_months_(long_months),
        short_days_(short_days),
        long_days_(long_days) {}

  const char *canonical_name_ = "en";
  const char *const *short_months_ = nullptr;
  const char *const *long_months_ = nullptr;
  const char *const *short_days_ = nullptr;
  const char *const *long_days_ = nullptr;
};

// Canonical name a locale string resolves to; "" and unknown -> "en".
std::string CanonicalizeLocale(const std::string &locale);

}  // namespace vxcore

#endif  // VXCORE_DATETIME_NAMES_H

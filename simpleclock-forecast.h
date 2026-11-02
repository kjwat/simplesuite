#ifndef SIMPLECLOCK_FORECAST_H
#define SIMPLECLOCK_FORECAST_H

#include <math.h>

#define FORECAST_MAX_DAYS 30

typedef struct {
    char date[11];
    int weather_code;
    int precipitation_chance; /* -1 means unavailable, never inferred from amount. */
    char high[24], low[24], rain[24];
} ForecastDay;

typedef struct {
    ForecastDay days[FORECAST_MAX_DAYS];
    int count;
    bool metric;
} ForecastData;

/* Open-Meteo's CSV avoids a JSON dependency for this small daily table. */
static bool forecast_parse(const char *response, int days, ForecastData *data) {
    const char *header = strstr(response, "\ntime,temperature_2m_max (");
    const char *row;
    ForecastData parsed = {0};
    if (!header || days < 1 || days > FORECAST_MAX_DAYS) return false;
    row = strchr(header + 1, '\n');
    if (!row) return false;
    parsed.metric = strstr(header, "temperature_2m_max (°C)") == header + 6;
    if (!parsed.metric && strstr(header, "temperature_2m_max (°F)") != header + 6)
        return false;
    const char *expected = parsed.metric
        ? "\ntime,temperature_2m_max (°C),temperature_2m_min (°C),precipitation_sum (mm),weather_code (wmo code)"
        : "\ntime,temperature_2m_max (°F),temperature_2m_min (°F),precipitation_sum (inch),weather_code (wmo code)";
    char expected_header[256];
    bool has_chance = strstr(header, ",precipitation_probability_max (%)") == header + strlen(expected);
    snprintf(expected_header, sizeof expected_header, "%s%s", expected,
             has_chance ? ",precipitation_probability_max (%)" : "");
    expected = expected_header;
    if ((size_t)(row - header) != strlen(expected) &&
        !((size_t)(row - header) == strlen(expected) + 1 && row[-1] == '\r')) return false;
    if (strncmp(header, expected, strlen(expected))) return false;
    for (row++; *row && parsed.count < days; ) {
        ForecastDay *day = &parsed.days[parsed.count];
        int year, month, date, consumed = 0;
        char code[24], chance[24];
        day->precipitation_chance = -1;
        int fields;
        if (has_chance) {
            fields = sscanf(row, "%4d-%2d-%2d,%23[^,],%23[^,],%23[^,],%23[^,],%23[^\r\n]%n",
                            &year, &month, &date, day->high, day->low, day->rain,
                            code, chance, &consumed);
            if (fields != 8) return false;
            if (strcmp(chance, "NaN") && strcmp(chance, "null")) {
                char *end;
                long value = strtol(chance, &end, 10);
                if (end == chance || *end || value < 0 || value > 100) return false;
                day->precipitation_chance = (int)value;
            }
        } else {
            fields = sscanf(row, "%4d-%2d-%2d,%23[^,],%23[^,],%23[^,],%23[^\r\n]%n",
                   &year, &month, &date, day->high, day->low, day->rain,
                   code, &consumed);
            if (fields != 7) return false;
        }
        if (consumed <= 0 ||
            month < 1 || month > 12 || date < 1 || date > 31 ||
            (row[consumed] && row[consumed] != '\r' && row[consumed] != '\n'))
            return false;
        if (strlen(row) < 10 || row[4] != '-' || row[7] != '-') return false;
        memcpy(day->date, row, 10); day->date[10] = 0;
        for (int i = 0; i < 10; i++)
            if (i != 4 && i != 7 && !isdigit((unsigned char)day->date[i])) return false;
        char *values[] = {day->high, day->low, day->rain};
        for (size_t i = 0; i < 3; i++) {
            if (!strcmp(values[i], "NaN") || !strcmp(values[i], "null") || !strcmp(values[i], "--")) {
                strcpy(values[i], "--"); continue;
            }
            char *end;
            double value = strtod(values[i], &end);
            if (end == values[i] || *end || !isfinite(value)) return false;
        }
        if (!strcmp(code, "NaN") || !strcmp(code, "null")) {
            day->weather_code = -1;
        } else {
            char *end;
            long value = strtol(code, &end, 10);
            if (end == code || *end || value < 0 || value > 99) return false;
            day->weather_code = (int)value;
        }
        if (parsed.count && strcmp(day->date, parsed.days[parsed.count - 1].date) <= 0)
            return false;
        parsed.count++;
        row += consumed;
        while (*row == '\r' || *row == '\n') row++;
    }
    if (parsed.count != days) return false;
    *data = parsed;
    return true;
}

typedef struct { char *text; size_t length, capacity; } ForecastReply;

static size_t forecast_collect(char *text, size_t size, size_t count, void *opaque) {
    ForecastReply *reply = opaque;
    size_t bytes = size * count;
    if (size && bytes / size != count) return 0;
    if (bytes >= reply->capacity - reply->length) return 0;
    memcpy(reply->text + reply->length, text, bytes);
    reply->length += bytes;
    reply->text[reply->length] = 0;
    return bytes;
}

static bool forecast_get(CURL *curl, const char *url, ForecastReply *reply) {
    reply->length = 0; reply->text[0] = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SimpleClock/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, forecast_collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, reply);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    return curl_easy_perform(curl) == CURLE_OK;
}

/* wttr's nearest_area coordinates resolve exactly the same configured place
 * (or IP location) used by the existing current-conditions view. */
static bool forecast_coordinate(const char *area, const char *key,
                                double limit, double *value) {
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\"", key);
    const char *p = strstr(area, pattern);
    char *end;
    if (!p || !(p = strchr(p + strlen(pattern), ':'))) return false;
    for (p++; isspace((unsigned char)*p); p++);
    if (*p++ != '"') return false;
    *value = strtod(p, &end);
    return end != p && *end == '"' && isfinite(*value) &&
           *value >= -limit && *value <= limit;
}

static bool forecast_metric(const char *area) {
    const char *units = getenv("SIMPLECLOCK_UNITS");
    if (units && (!strcasecmp(units, "metric") || !strcasecmp(units, "c") ||
                  !strcasecmp(units, "m") || !strcasecmp(units, "si"))) return true;
    if (units && (!strcasecmp(units, "imperial") || !strcasecmp(units, "f") ||
                  !strcasecmp(units, "u") || !strcasecmp(units, "us"))) return false;
    return strstr(area, "United States") == NULL;
}

/* An ensemble frequency is a model estimate, not a calibrated probability.
 * Count members predicting at least 0.1 mm in the location's local day. */
static bool forecast_ensemble_chances(const char *csv, ForecastData *data) {
    const char *p = strstr(csv, "\ntime,");
    if (!p) return false;
    p += 6;
    for (int member = 0; member < 51; member++) {
        char column[64];
        if (!member) snprintf(column, sizeof column, "precipitation_sum (mm)");
        else snprintf(column, sizeof column, "precipitation_sum_member%02d (mm)", member);
        size_t length = strlen(column);
        if (strncmp(p, column, length)) return false;
        p += length;
        if (member < 50) { if (*p++ != ',') return false; }
    }
    if (*p == '\r') p++;
    if (*p++ != '\n') return false;
    int chances[FORECAST_MAX_DAYS];
    for (int day = 0; day < data->count; day++) {
        if (strncmp(p, data->days[day].date, 10) || p[10] != ',') return false;
        p += 11;
        int wet = 0, valid = 0;
        for (int member = 0; member < 51; member++) {
            char *end;
            double amount = strtod(p, &end);
            if (p == end || (*end != ',' && *end != '\r' && *end != '\n' && *end)) return false;
            if (isfinite(amount) && amount >= 0) {
                valid++; if (amount >= 0.1) wet++;
            }
            p = end;
            if (member < 50) { if (*p++ != ',') return false; }
        }
        chances[day] = valid == 51 ? (wet * 100 + 25) / 51 : -1;
        if (*p == '\r') p++;
        if (*p == '\n') p++;
        else if (day + 1 < data->count || *p) return false;
    }
    for (int day = 0; day < data->count; day++)
        data->days[day].precipitation_chance = chances[day];
    return true;
}

static bool forecast_write_data(int fd, const ForecastData *data) {
    if (dprintf(fd, "\ntime,temperature_2m_max (°%s),temperature_2m_min (°%s),"
                "precipitation_sum (%s),weather_code (wmo code),precipitation_probability_max (%%)\n",
                data->metric ? "C" : "F", data->metric ? "C" : "F",
                data->metric ? "mm" : "inch") < 0) return false;
    for (int i = 0; i < data->count; i++) {
        const ForecastDay *day = &data->days[i];
        char code[16] = "NaN", chance[16] = "NaN";
        if (day->weather_code >= 0) snprintf(code, sizeof code, "%d", day->weather_code);
        if (day->precipitation_chance >= 0) snprintf(chance, sizeof chance, "%d", day->precipitation_chance);
        if (dprintf(fd, "%s,%s,%s,%s,%s,%s\n", day->date, day->high, day->low,
                    day->rain, code, chance) < 0) return false;
    }
    return true;
}

static int forecast_fetch_to_fd(int fd, int days) {
    int result = 1;
    CURL *curl = NULL;
    char url[2048];
    char *escaped = NULL;
    ForecastReply reply = {.capacity = 262144};
    reply.text = malloc(reply.capacity);
    if (!reply.text) return 1;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) { free(reply.text); return 1; }
    curl = curl_easy_init();
    if (!curl) goto done;
    const char *location = getenv("SIMPLECLOCK_LOCATION");
    escaped = curl_easy_escape(curl, location ? location : "", 0);
    if (!escaped || snprintf(url, sizeof url, "https://wttr.in/%s?format=j1", escaped)
                    >= (int)sizeof url) goto done;
    if (!forecast_get(curl, url, &reply)) goto done;
    const char *area = strstr(reply.text, "\"nearest_area\"");
    double latitude, longitude;
    if (!area || !forecast_coordinate(area, "latitude", 90, &latitude) ||
        !forecast_coordinate(area, "longitude", 180, &longitude)) goto done;
    bool metric = forecast_metric(area);
    if (snprintf(url, sizeof url,
        "https://%s.open-meteo.com/v1/%s?latitude=%.6f&longitude=%.6f"
        "&daily=temperature_2m_max,temperature_2m_min,precipitation_sum,weather_code%s"
        "&forecast_days=%d&timezone=auto&format=csv&temperature_unit=%s&precipitation_unit=%s%s",
        days == 30 ? "seasonal-api" : "api", days == 30 ? "seasonal" : "forecast",
        latitude, longitude, days == 30 ? "" : ",precipitation_probability_max",
        days, metric ? "celsius" : "fahrenheit",
        metric ? "mm" : "inch", days == 30 ? "&models=ecmwf_ec46_ensemble_mean" : "")
        >= (int)sizeof url) goto done;
    if (!forecast_get(curl, url, &reply)) goto done;
    ForecastData checked;
    if (!forecast_parse(reply.text, days, &checked)) goto done;
    if (days == 30) {
        if (snprintf(url, sizeof url,
            "https://seasonal-api.open-meteo.com/v1/seasonal?latitude=%.6f&longitude=%.6f"
            "&daily=precipitation_sum&forecast_days=30&timezone=auto&format=csv"
            "&precipitation_unit=mm&models=ecmwf_ec46", latitude, longitude) >= (int)sizeof url) goto done;
        if (forecast_get(curl, url, &reply))
            (void)forecast_ensemble_chances(reply.text, &checked);
        result = forecast_write_data(fd, &checked) ? 0 : 1;
        goto done;
    }
    size_t sent = 0;
    while (sent < reply.length) {
        ssize_t n = write(fd, reply.text + sent, reply.length - sent);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        sent += (size_t)n;
    }
    result = 0;
done:
    curl_free(escaped);
    if (curl) curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(reply.text);
    return result;
}
#endif

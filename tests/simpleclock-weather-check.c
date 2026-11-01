#define main simpleclock_program_main
#include "../simpleclock.c"
#undef main

#include <assert.h>

static void check_forecasts(void)
{
    ForecastData data;
    char csv[8192] = "latitude,longitude\n40.8,-73.0\n\n"
        "time,temperature_2m_max (°F),temperature_2m_min (°F),precipitation_sum (inch),weather_code (wmo code)\n";
    for (int i = 1; i <= 30; i++) {
        char row[128];
        snprintf(row, sizeof row, "2026-11-%02d,55.2,40.1,0.100,2\n", i);
        strcat(csv, row);
    }
    const int ranges[] = {5, 14, 30};
    for (int i = 0; i < 3; i++) {
        assert(forecast_parse(csv, ranges[i], &data));
        assert(data.count == ranges[i] && !data.metric);
        assert(data.days[0].weather_code == 2);
        assert(!strcmp(data.days[0].date, "2026-11-01"));
        assert(!strcmp(data.days[ranges[i] - 1].high, "55.2"));
    }
    assert(!forecast_parse(csv, 31, &data));
    assert(!forecast_parse("error reply", 5, &data));
    const char *metric = "metadata\n\ntime,temperature_2m_max (°C),temperature_2m_min (°C),precipitation_sum (mm),weather_code (wmo code)\n"
        "2026-11-01,12.3,-1.5,NaN,NaN\n";
    assert(forecast_parse(metric, 1, &data) && data.metric);
    assert(!strcmp(data.days[0].rain, "--"));
    assert(data.days[0].weather_code == -1);
    assert(!forecast_parse(metric, 5, &data)); /* incomplete reply */
    char *number = strstr(csv, "55.2"); assert(number);
    memcpy(number, "oops", 4);
    assert(!forecast_parse(csv, 5, &data));

    const struct { int code; WeatherScene scene; } cases[] = {
        {0, WEATHER_SCENE_SUN}, {1, WEATHER_SCENE_SUN},
        {2, WEATHER_SCENE_PARTLY_CLOUDY}, {3, WEATHER_SCENE_CLOUDY},
        {45, WEATHER_SCENE_FOG}, {53, WEATHER_SCENE_RAIN},
        {57, WEATHER_SCENE_SLEET}, {65, WEATHER_SCENE_RAIN},
        {67, WEATHER_SCENE_SLEET}, {75, WEATHER_SCENE_SNOW},
        {81, WEATHER_SCENE_RAIN}, {86, WEATHER_SCENE_SNOW},
        {99, WEATHER_SCENE_STORM}, {-1, WEATHER_SCENE_UNKNOWN}
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
        assert(weather_scene_for_condition(forecast_condition(cases[i].code)) == cases[i].scene);

    assert(forecast_art_rows(5) == 7);
    assert(forecast_art_rows(14) == 3);
    assert(forecast_art_rows(30) == 3);
    COLS = 100; LINES = 28;
    assert(forecast_card_height(5) > forecast_card_height(14));
    assert(forecast_card_height(14) > forecast_card_height(30));
    assert(forecast_page_days(5) == 2 && forecast_page_days(14) == 3 && forecast_page_days(30) == 4);
    for (int scene = WEATHER_SCENE_SUN; scene <= WEATHER_SCENE_UNKNOWN; scene++) {
        for (int range = 5; range <= 30; range += range == 5 ? 9 : 16) {
            const char *const *art = forecast_art((WeatherScene)scene, range);
            for (int row = 0; row < forecast_art_rows(range); row++) assert(art[row] && *art[row]);
        }
    }

    double coordinate;
    assert(forecast_coordinate("{\"latitude\": \"40.831\"}", "latitude", 90, &coordinate));
    assert(coordinate > 40.8 && coordinate < 40.9);
    assert(!forecast_coordinate("{\"latitude\": \"999\"}", "latitude", 90, &coordinate));
    assert(!forecast_coordinate("{\"latitude\": \"NaN\"}", "latitude", 90, &coordinate));
    assert(!forecast_coordinate("{}", "latitude", 90, &coordinate));
    setenv("SIMPLECLOCK_UNITS", "metric", 1);
    assert(forecast_metric("United States"));
    setenv("SIMPLECLOCK_UNITS", "imperial", 1);
    assert(!forecast_metric("Canada"));
    unsetenv("SIMPLECLOCK_UNITS");
    assert(!forecast_metric("United States"));
    assert(forecast_metric("Canada"));
}

int main(void)
{
    check_forecasts();
    WeatherData weather;
    WeatherState state;
    CURL *curl;
    char url[2048];

    assert(parse_weather_record(
        "New York|Partly cloudy|+72°F|+70°F|55%|↗6mph|0.00 in\n",
        &weather));
    assert(!strcmp(weather.location, "New York"));
    assert(!strcmp(weather.condition, "Partly cloudy"));
    assert(!strcmp(weather.temperature, "+72°F"));
    assert(!strcmp(weather.feels_like, "+70°F"));
    assert(!strcmp(weather.humidity, "55%"));
    assert(!strcmp(weather.precipitation, "0.00 in"));
    assert(weather.scene == WEATHER_SCENE_PARTLY_CLOUDY);

    assert(weather_scene_for_condition("Clear") == WEATHER_SCENE_SUN);
    assert(weather_scene_for_condition("Overcast") == WEATHER_SCENE_CLOUDY);
    assert(weather_scene_for_condition("Light rain shower") == WEATHER_SCENE_RAIN);
    assert(weather_scene_for_condition("Thunderstorms") == WEATHER_SCENE_STORM);
    assert(weather_scene_for_condition("Blowing snow") == WEATHER_SCENE_SNOW);
    assert(weather_scene_for_condition("Ice pellets") == WEATHER_SCENE_SLEET);
    assert(weather_scene_for_condition("Haze") == WEATHER_SCENE_FOG);
    assert(weather_scene_for_condition("Something new") == WEATHER_SCENE_UNKNOWN);
    assert(!parse_weather_record("missing|fields", &weather));
    assert(!parse_weather_record("||||||", &weather));

    weather_state_init(&state);
    assert(weather_needs_refresh(&state, 1000));
    state.status = WEATHER_READY;
    state.fetched_at = 950;
    assert(!weather_needs_refresh(&state, 1000));
    assert(weather_needs_refresh(&state, 950 + WEATHER_CACHE_SECONDS));

    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    curl = curl_easy_init();
    assert(curl);
    setenv("SIMPLECLOCK_LOCATION", "New York, NY", 1);
    setenv("SIMPLECLOCK_UNITS", "metric", 1);
    assert(weather_build_url(curl, url, sizeof url));
    assert(strstr(url, "https://wttr.in/New%20York%2C%20NY?m&format="));
    assert(strstr(url, "%25C"));
    unsetenv("SIMPLECLOCK_LOCATION");
    unsetenv("SIMPLECLOCK_UNITS");
    curl_easy_cleanup(curl);
    curl_global_cleanup();

    for (int scene = WEATHER_SCENE_SUN; scene <= WEATHER_SCENE_UNKNOWN; scene++) {
        const char *const *art = weather_art_for_scene((WeatherScene)scene);

        assert(art);
        for (int row = 0; row < WEATHER_ART_ROWS; row++) {
            assert(art[row]);
            assert(*art[row]);
        }
    }

    return 0;
}

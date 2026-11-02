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
        assert(data.days[0].precipitation_chance == -1);
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

    const char *chance_header = "metadata\n\ntime,temperature_2m_max (°F),temperature_2m_min (°F),precipitation_sum (inch),weather_code (wmo code),precipitation_probability_max (%)\n";
    const char *probabilities[] = {"0", "60", "100", "NaN", "null", "-1", "101", "rain"};
    const int expected_chances[] = {0, 60, 100, -1, -1};
    for (size_t i = 0; i < sizeof probabilities / sizeof *probabilities; i++) {
        char response[1024];
        snprintf(response, sizeof response, "%s2026-11-01,55.2,40.1,0.100,2,%s\n", chance_header, probabilities[i]);
        bool valid = forecast_parse(response, 1, &data);
        assert(valid == (i < 5));
        if (valid) assert(data.days[0].precipitation_chance == expected_chances[i]);
    }

    char ensemble[8192] = "metadata\n\ntime,";
    for (int member = 0; member < 51; member++) {
        char column[80];
        if (!member) snprintf(column, sizeof column, "precipitation_sum (mm)");
        else snprintf(column, sizeof column, "precipitation_sum_member%02d (mm)", member);
        strcat(ensemble, column); strcat(ensemble, member == 50 ? "\n" : ",");
    }
    size_t header_length = strlen(ensemble);
    data.count = 1; strcpy(data.days[0].date, "2026-11-01");
    const int expected_model_chances[] = {0, 51, 100, -1};
    for (int scenario = 0; scenario < 4; scenario++) {
        ensemble[header_length] = 0;
        strcat(ensemble, "2026-11-01,");
        for (int member = 0; member < 51; member++) {
            strcat(ensemble, scenario == 3 && member == 50 ? "NaN" :
                   scenario == 2 || (scenario == 1 && member < 26) ? "0.10" : "0.09");
            strcat(ensemble, member == 50 ? "\n" : ",");
        }
        assert(forecast_ensemble_chances(ensemble, &data));
        assert(data.days[0].precipitation_chance == expected_model_chances[scenario]);
    }
    data.days[0].precipitation_chance = 60;
    ensemble[strlen(ensemble) - 3] = 0; /* incomplete member */
    assert(!forecast_ensemble_chances(ensemble, &data));
    assert(data.days[0].precipitation_chance == 60);
    /* Worker serialization preserves calculated probabilities and missing data. */
    data.metric = true; data.days[0].weather_code = -1;
    strcpy(data.days[0].high, "--"); strcpy(data.days[0].low, "2.0"); strcpy(data.days[0].rain, "0.10");
    FILE *serialized = tmpfile(); assert(serialized);
    assert(forecast_write_data(fileno(serialized), &data));
    rewind(serialized);
    char roundtrip[1024]; size_t n = fread(roundtrip, 1, sizeof roundtrip - 1, serialized);
    roundtrip[n] = 0; fclose(serialized);
    assert(forecast_parse(roundtrip, 1, &data));
    assert(data.days[0].precipitation_chance == 60 && !strcmp(data.days[0].high, "--"));

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

#define main simpleclock_program_main
#include "../simpleclock.c"
#undef main

#include <assert.h>

int main(void)
{
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

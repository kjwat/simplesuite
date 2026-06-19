#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int WIDTH = 900;
int HEIGHT = 650;

const float PLAYER_SPEED = 260.0f;
const float ENEMY_SPEED = 105.0f;
const float PLAYER_RADIUS = 10.0f;
const float ENEMY_RADIUS = 10.0f;
const float TREASURE_RADIUS = 7.0f;
const int TREASURES_TO_WIN = 20;

struct ScoreEntry {
    std::string name;
    float time;
};

float dist(sf::Vector2f a, sf::Vector2f b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

sf::Vector2f random_pos(float margin = 30.0f) {
    int usable_w = std::max(1, WIDTH - (int)(margin * 2));
    int usable_h = std::max(1, HEIGHT - (int)(margin * 2));

    return {
        margin + static_cast<float>(std::rand() % usable_w),
        margin + static_cast<float>(std::rand() % usable_h)
    };
}

sf::Vector2f normalize(sf::Vector2f v) {
    float len = std::sqrt(v.x * v.x + v.y * v.y);
    if (len == 0) return {0, 0};
    return {v.x / len, v.y / len};
}

void center_text(sf::Text &text, float x, float y) {
    sf::FloatRect b = text.getLocalBounds();
    text.setOrigin(b.left + b.width / 2.f, b.top + b.height / 2.f);
    text.setPosition(x, y);
}

std::string score_dir() {
    const char *home = std::getenv("HOME");
    if (!home) home = ".";
    return std::string(home) + "/.local/share/dotfrolic";
}

std::string score_file() {
    return score_dir() + "/top_times.txt";
}

void ensure_score_dir() {
    const char *home = std::getenv("HOME");
    if (!home) return;

    std::string p = std::string(home) + "/.local";
    mkdir(p.c_str(), 0755);

    p += "/share";
    mkdir(p.c_str(), 0755);

    mkdir(score_dir().c_str(), 0755);
}

std::vector<ScoreEntry> load_scores() {
    ensure_score_dir();

    std::vector<ScoreEntry> scores;
    std::ifstream in(score_file());

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;

        ScoreEntry e;
        size_t bar = line.find('|');

        if (bar == std::string::npos) {
            e.name = "ANON";
            e.time = std::stof(line);
        } else {
            e.name = line.substr(0, bar);
            e.time = std::stof(line.substr(bar + 1));
        }

        if (e.name.empty()) e.name = "ANON";
        scores.push_back(e);
    }

    std::sort(scores.begin(), scores.end(),
              [](const ScoreEntry &a, const ScoreEntry &b) {
                  return a.time < b.time;
              });

    if (scores.size() > 10)
        scores.resize(10);

    return scores;
}

void save_score(const std::string &name, float time) {
    auto scores = load_scores();

    ScoreEntry e;
    e.name = name.empty() ? "ANON" : name;
    e.time = time;
    scores.push_back(e);

    std::sort(scores.begin(), scores.end(),
              [](const ScoreEntry &a, const ScoreEntry &b) {
                  return a.time < b.time;
              });

    if (scores.size() > 10)
        scores.resize(10);

    ensure_score_dir();

    std::ofstream out(score_file());
    for (auto &s : scores)
        out << s.name << "|" << s.time << "\n";
}

void reset_game(
    sf::CircleShape &player,
    sf::CircleShape &treasure,
    sf::CircleShape &magnet,
    sf::CircleShape &slowmo,
    std::vector<sf::CircleShape> &enemies,
    int &score,
    bool &gameOver,
    bool &won,
    bool &started,
    bool &magnetActive,
    bool &slowActive,
    float &magnetTimer,
    float &slowTimer,
    sf::Clock &runClock
) {
    player.setPosition(WIDTH / 2.0f, HEIGHT / 2.0f);
    treasure.setPosition(random_pos());
    magnet.setPosition(random_pos());
    slowmo.setPosition(random_pos());

    enemies.clear();
    for (int i = 0; i < 4; i++) {
        sf::CircleShape e(ENEMY_RADIUS);
        e.setFillColor(sf::Color::Red);
        e.setOrigin(ENEMY_RADIUS, ENEMY_RADIUS);
        e.setPosition(random_pos());
        enemies.push_back(e);
    }

    score = 0;
    gameOver = false;
    won = false;
    started = false;
    magnetActive = false;
    slowActive = false;
    magnetTimer = 0;
    slowTimer = 0;
    runClock.restart();
}

int main() {
    std::srand(static_cast<unsigned>(std::time(nullptr)));

    sf::VideoMode desktop = sf::VideoMode::getDesktopMode();
    WIDTH = desktop.width;
    HEIGHT = desktop.height;

    sf::RenderWindow window(desktop, "DotFrolic", sf::Style::Fullscreen);
    window.setFramerateLimit(60);

    sf::Font font;
    bool hasFont = font.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");

    sf::CircleShape player(PLAYER_RADIUS);
    player.setFillColor(sf::Color::Blue);
    player.setOrigin(PLAYER_RADIUS, PLAYER_RADIUS);

    sf::CircleShape treasure(TREASURE_RADIUS);
    treasure.setFillColor(sf::Color::Yellow);
    treasure.setOrigin(TREASURE_RADIUS, TREASURE_RADIUS);

    sf::CircleShape magnet(9);
    magnet.setFillColor(sf::Color(180, 0, 255));
    magnet.setOrigin(9, 9);

    sf::CircleShape slowmo(9);
    slowmo.setFillColor(sf::Color::White);
    slowmo.setOrigin(9, 9);

    std::vector<sf::CircleShape> enemies;

    int score = 0;
    bool gameOver = false;
    bool won = false;
    bool started = false;
    bool showScores = false;
    bool enteringName = false;

    std::string playerName;
    float finalTime = 0.0f;

    bool magnetActive = false;
    bool slowActive = false;
    float magnetTimer = 0;
    float slowTimer = 0;

    sf::Clock clock;
    sf::Clock runClock;

    reset_game(player, treasure, magnet, slowmo, enemies, score, gameOver, won,
               started, magnetActive, slowActive, magnetTimer, slowTimer, runClock);

    while (window.isOpen()) {
        float dt = clock.restart().asSeconds();

        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed)
                window.close();

            if (enteringName && event.type == sf::Event::TextEntered) {
                if (event.text.unicode == 8) {
                    if (!playerName.empty())
                        playerName.pop_back();
                } else if (event.text.unicode >= 32 &&
                           event.text.unicode <= 126 &&
                           playerName.size() < 12) {
                    playerName += static_cast<char>(event.text.unicode);
                }
            }

            if (event.type == sf::Event::KeyPressed) {
                if (enteringName) {
                    if (event.key.code == sf::Keyboard::Enter) {
                        save_score(playerName.empty() ? "ANON" : playerName, finalTime);
                        enteringName = false;
                        showScores = true;
                    }

                    if (event.key.code == sf::Keyboard::Escape) {
                        playerName = "ANON";
                        save_score(playerName, finalTime);
                        enteringName = false;
                        showScores = true;
                    }

                    continue;
                }

                if (!started && event.key.code == sf::Keyboard::Space) {
                    started = true;
                    runClock.restart();
                }

                if ((gameOver || won) && event.key.code == sf::Keyboard::R) {
                    reset_game(player, treasure, magnet, slowmo, enemies, score, gameOver, won,
                               started, magnetActive, slowActive, magnetTimer, slowTimer, runClock);
                    showScores = false;
                    enteringName = false;
                    playerName.clear();
                    finalTime = 0.0f;
                }

                if (event.key.code == sf::Keyboard::Escape || event.key.code == sf::Keyboard::Q)
                    window.close();
            }
        }

        if (started && !gameOver && !won) {
            sf::Vector2f move(0, 0);

            if (sf::Keyboard::isKeyPressed(sf::Keyboard::W) || sf::Keyboard::isKeyPressed(sf::Keyboard::Up))
                move.y -= 1;
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::S) || sf::Keyboard::isKeyPressed(sf::Keyboard::Down))
                move.y += 1;
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::A) || sf::Keyboard::isKeyPressed(sf::Keyboard::Left))
                move.x -= 1;
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::D) || sf::Keyboard::isKeyPressed(sf::Keyboard::Right))
                move.x += 1;

            move = normalize(move);
            player.move(move.x * PLAYER_SPEED * dt, move.y * PLAYER_SPEED * dt);

            sf::Vector2f p = player.getPosition();
            p.x = std::max(PLAYER_RADIUS, std::min((float)WIDTH - PLAYER_RADIUS, p.x));
            p.y = std::max(PLAYER_RADIUS, std::min((float)HEIGHT - PLAYER_RADIUS, p.y));
            player.setPosition(p);

            if (magnetActive) {
                sf::Vector2f dir = normalize(player.getPosition() - treasure.getPosition());
                treasure.move(dir.x * 220.0f * dt, dir.y * 220.0f * dt);
                magnetTimer -= dt;
                if (magnetTimer <= 0) magnetActive = false;
            }

            if (slowActive) {
                slowTimer -= dt;
                if (slowTimer <= 0) slowActive = false;
            }

            for (auto &e : enemies) {
                sf::Vector2f dir = normalize(player.getPosition() - e.getPosition());
                float speed = slowActive ? ENEMY_SPEED * 0.45f : ENEMY_SPEED;
                e.move(dir.x * speed * dt, dir.y * speed * dt);

                if (dist(player.getPosition(), e.getPosition()) < PLAYER_RADIUS + ENEMY_RADIUS) {
                    gameOver = true;
                    showScores = true;
                }
            }

            if (dist(player.getPosition(), treasure.getPosition()) < PLAYER_RADIUS + TREASURE_RADIUS) {
                score++;
                treasure.setPosition(random_pos());

                if (score % 5 == 0) {
                    sf::CircleShape e(ENEMY_RADIUS);
                    e.setFillColor(sf::Color::Red);
                    e.setOrigin(ENEMY_RADIUS, ENEMY_RADIUS);
                    e.setPosition(random_pos());
                    enemies.push_back(e);
                }

                if (score >= TREASURES_TO_WIN) {
                    won = true;
                    finalTime = runClock.getElapsedTime().asSeconds();
                    enteringName = true;
                    playerName.clear();
                }
            }

            if (dist(player.getPosition(), magnet.getPosition()) < PLAYER_RADIUS + 9) {
                magnetActive = true;
                magnetTimer = 6.0f;
                magnet.setPosition(random_pos());
            }

            if (dist(player.getPosition(), slowmo.getPosition()) < PLAYER_RADIUS + 9) {
                slowActive = true;
                slowTimer = 5.0f;
                slowmo.setPosition(random_pos());
            }
        }

        window.clear(sf::Color(20, 20, 25));

        if (!started) {
            if (hasFont) {
                float cx = window.getSize().x / 2.f;
                float cy = window.getSize().y / 2.f;

                sf::Text title("DOTFROLIC", font, 52);
                title.setFillColor(sf::Color::Cyan);
                center_text(title, cx, cy - 180);

                sf::Text sub(
                    "Blue dot. Yellow treasure. Red death.\n"
                    "Purple = magnet. White = slow motion.\n"
                    "Press SPACE to begin.",
                    font,
                    24
                );
                sub.setFillColor(sf::Color::White);
                center_text(sub, cx, cy - 80);

                window.draw(title);
                window.draw(sub);
            }
        } else {
            window.draw(treasure);
            window.draw(magnet);
            window.draw(slowmo);

            for (auto &e : enemies)
                window.draw(e);

            window.draw(player);

            if (hasFont) {
                std::ostringstream ss;
                ss << "Score: " << score << "/" << TREASURES_TO_WIN
                   << "   Time: " << (int)runClock.getElapsedTime().asSeconds();

                if (magnetActive) ss << "   MAGNET";
                if (slowActive) ss << "   SLOWMO";

                sf::Text hud(ss.str(), font, 20);
                hud.setFillColor(sf::Color::White);
                hud.setPosition(15, 12);
                window.draw(hud);

                float cx = window.getSize().x / 2.f;
                float top = 155.f;

                if (enteringName) {
                    sf::Text win("You Win!", font, 64);
                    win.setFillColor(sf::Color::Green);
                    center_text(win, cx, top);
                    window.draw(win);

                    sf::Text prompt("Enter your name:", font, 40);
                    prompt.setFillColor(sf::Color::Yellow);
                    center_text(prompt, cx, top + 95.f);
                    window.draw(prompt);

                    sf::Text name("> " + playerName, font, 38);
                    name.setFillColor(sf::Color::White);
                    center_text(name, cx, top + 155.f);
                    window.draw(name);

                    sf::Text done("Press Enter", font, 28);
                    done.setFillColor(sf::Color::White);
                    center_text(done, cx, top + 225.f);
                    window.draw(done);
                } else if (showScores) {
                    sf::Text result(won ? "You Win!" : "Red Dot Got You", font, 64);
                    result.setFillColor(won ? sf::Color::Green : sf::Color::Red);
                    center_text(result, cx, top);
                    window.draw(result);

                    sf::Text heading("Top 10 Fastest Wins", font, 40);
                    heading.setFillColor(sf::Color::Yellow);
                    center_text(heading, cx, top + 85.f);
                    window.draw(heading);

                    auto scores = load_scores();

                    if (scores.empty()) {
                        sf::Text none("No wins recorded yet", font, 32);
                        none.setFillColor(sf::Color::White);
                        center_text(none, cx, top + 150.f);
                        window.draw(none);
                    } else {
                        for (size_t i = 0; i < scores.size(); i++) {
                            std::ostringstream row;
                            row << (i + 1) << ". "
                                << scores[i].name
                                << "   "
                                << (int)scores[i].time
                                << " sec";

                            sf::Text line(row.str(), font, 32);
                            line.setFillColor(sf::Color::White);
                            center_text(line, cx, top + 150.f + (float)i * 38.f);
                            window.draw(line);
                        }
                    }

                    sf::Text restart("Press R to restart", font, 36);
                    restart.setFillColor(sf::Color::White);
                    center_text(restart, cx, top + 590.f);
                    window.draw(restart);
                }
            }
        }

        window.display();
    }

    return 0;
}

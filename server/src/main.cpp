// Claude Pad — M5Stack Core2
//
//  Claude Code (hook http) --POST /event--> Core2 (affichage + vibration,
//                                           réponse de Claude en direct)
//  Claude Code (hook http) --POST /hook---> Core2 (permission, question, plan)
//                          <--décision----- (choix touché à l'écran)
//  pad-usage.py            --POST /usage--> Core2 (contexte, tokens, prix)
//
//  Claude Code envoie lui-même les événements : rien à lancer sur le PC (sauf
//  pad-usage.py, qui lit le transcript). Pour une demande, le pad garde la
//  connexion ouverte et répond avec la décision au format des hooks : pas de
//  clavier simulé, ça marche dans le terminal comme dans l'app de bureau. Les choix se font sur l'écran tactile ; les zones A / B / C
//  sous l'écran ouvrent les vues Sessions / Conso / Direct.
//
//  Wi-Fi : réglé depuis un téléphone. Sans réseau enregistré (ou s'il ne
//  répond pas, ou si on touche l'écran au démarrage), le pad ouvre le point
//  d'accès « Claude-Pad » et sa page de réglage (portail captif).

#include <M5Unified.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <algorithm>
#include <vector>

// Facultatif : réseau par défaut tant qu'aucun n'a été choisi sur le pad
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef PAD_HOSTNAME
#define PAD_HOSTNAME "claude-pad"  // http://claude-pad.local
#endif
constexpr const char* AP_NAME = "Claude-Pad";        // point d'accès de réglage

// ---------------------------------------------------------------- état ----

enum class State { Idle, Thinking, Working, Waiting, Done };

struct Ui {
  State state = State::Idle;
  String project;   // nom du dossier de travail
  String title;     // ex. nom de l'outil
  String detail;    // ex. commande bash, fichier, message de notification
  uint32_t since = 0;
  bool dirty = true;
} ui;

WebServer server(80);

String lastTool;  // dernier outil lancé, affiché quand Claude attend une permission
String lastWhat;  // sa commande / son fichier

// Demande en cours (permission, question, plan) et ses choix à l'écran.
// `id` change à chaque demande ; vide quand rien n'attend de réponse.
enum class Ask { None, Permission, Question, Plan, Other };
enum class Act { Allow, Always, Deny, Pick, Toggle, Submit };
struct Choice { String label; Act act; bool on; };  // on : case cochée (choix multiple)
struct Prompt {
  Ask kind = Ask::None;
  String id;
  Choice choices[7];
  int count = 0;
  JsonDocument questions;  // AskUserQuestion : la liste complète
  int qIndex = 0, qCount = 0;
  String answers[4];       // une réponse par question
} ask;

// Demande en attente : la connexion du hook reste ouverte jusqu'au choix
// à l'écran (Claude Code attend la réponse), ou jusqu'à l'abandon.
struct Held { WiFiClient client; JsonDocument event; uint32_t since = 0; } held;
uint32_t promptSeq = 0;
constexpr uint32_t PROMPT_TIMEOUT_MS = 90000;  // ensuite : menu du PC
constexpr const char* DENY_REASON = "Refusé depuis le Claude Pad";

// Sessions Claude Code vues par le pad (identifiées par session_id), avec
// leur consommation envoyée par pad-usage.py
struct Session {
  String id, project, model;
  State state = State::Idle;
  uint32_t seen = 0;  // millis du dernier événement
  bool hasUsage = false;
  uint32_t context = 0, window = 0;
  uint64_t input = 0, output = 0, cacheRead = 0, cacheWrite = 0;
  float cost = 0;
};
constexpr int MAX_SESSIONS = 8;
constexpr uint32_t SESSION_STALE_MS = 12UL * 3600 * 1000;  // oubliée après 12 h sans nouvelles
Session sessions[MAX_SESSIONS];
String lastSessionId;  // session du dernier événement

// Vues : Direct (état de Claude), Sessions, Conso (de la session `focusId`)
enum class View { Live, Sessions, Usage };
View view = View::Live;
uint32_t viewSince = 0;
String focusId;
constexpr uint32_t VIEW_TIMEOUT_MS = 30000;  // retour au direct

// Réponse de Claude en cours d'affichage (hook MessageDisplay, par paquets
// de lignes). Les blocs de code sont remplacés par « [code] ».
struct Reply {
  String msgId;
  String text;                 // fin du message, nettoyée du Markdown
  bool inFence = false;        // dans un bloc ``` ... ```
  std::vector<String> lines;   // `text` coupé à la largeur de l'écran
  bool wrapped = false;        // `lines` à jour
  int scroll = 0;              // lignes remontées depuis le bas (0 : suit le direct)
  int count = 0;               // nombre de lignes au dernier affichage
} reply;
constexpr int REPLY_MAX = 6000;  // octets gardés (la fin du message)
constexpr int REPLY_LINE_H = 20, REPLY_LINES = 6, REPLY_Y = 102;

uint32_t vibrateUntil = 0;
bool lastWifiConnected = false;

// --------------------------------------------------------------- utils ----

static String basename(const String& path) {
  int i = path.lastIndexOf('/');
  if (i < 0) i = path.lastIndexOf('\\');
  return i >= 0 ? path.substring(i + 1) : path;
}

// Première clé non vide parmi `keys` (ex. command, file_path...)
static String firstOf(JsonVariantConst obj, std::initializer_list<const char*> keys) {
  for (const char* k : keys) {
    const char* v = obj[k];
    if (v && *v) return String(v);
  }
  return String();
}

static void setState(State s, const String& title, const String& detail = "") {
  if (s != ui.state) ui.since = millis();
  ui.state = s;
  ui.title = title;
  ui.detail = detail;
  ui.dirty = true;
  // Claude attend : on revient au direct pour montrer la demande
  if (s == State::Waiting) view = View::Live;
  // Hors attente, plus aucun menu n'est ouvert côté Claude
  if (s != State::Waiting) {
    ask.kind = Ask::None;
    ask.count = 0;
    ask.id = "";
  }
}

static void buzz(uint16_t ms) {
  M5.Power.setVibration(180);
  vibrateUntil = millis() + ms;
}

static void alertWaiting() {
  buzz(350);
  M5.Speaker.tone(1760, 80);
  delay(110);
  M5.Speaker.tone(2349, 120);
}

// -------------------------------------------------------------- rendu ----
//
// Tout est dessiné dans un sprite en PSRAM puis poussé d'un bloc : pas de
// scintillement. Les polices efont sont en UTF-8 (accents français inclus).

M5Canvas canvas(&M5.Display);

#define F_TEXT  (&fonts::efontJA_16)
#define F_BOLD  (&fonts::efontJA_16_b)
#define F_BIG   (&fonts::efontJA_24_b)

constexpr uint16_t C_BG     = 0x0841;  // #0B0B0F
constexpr uint16_t C_CARD   = 0x18E4;  // #1C1D24
constexpr uint16_t C_MUTED  = 0x8C71;  // #8A8D8F
constexpr uint16_t C_DIM    = 0x39E7;  // #3A3C3C
constexpr uint16_t C_TEXT   = 0xEF7D;  // #E8ECEC

constexpr uint32_t DIM_AFTER_MS = 5UL * 60 * 1000;  // veille écran

struct Theme { uint16_t accent; const char* label; };

static Theme themeFor(State s) {
  switch (s) {
    case State::Thinking: return {0x3C3E, "RÉFLEXION"};  // bleu   #3B82F6
    case State::Working:  return {0x2629, "TRAVAIL"};    // vert   #22C55E
    case State::Waiting:  return {0xFBA2, "À TOI !"};    // orange #F97316
    case State::Done:     return {0x15D4, "TERMINÉ"};    // teal   #14B8A6
    default:              return {0x6B92, "EN VEILLE"};  // gris   #6B7280
  }
}

static bool animated(State s) {
  return s == State::Thinking || s == State::Working || s == State::Waiting;
}

// Mélange deux couleurs RGB565 (t = 0..255 vers b)
static uint16_t blend(uint16_t a, uint16_t b, uint8_t t) {
  int r = ((a >> 11) * (255 - t) + (b >> 11) * t) / 255;
  int g = (((a >> 5) & 63) * (255 - t) + ((b >> 5) & 63) * t) / 255;
  int bl = ((a & 31) * (255 - t) + (b & 31) * t) / 255;
  return (r << 11) | (g << 5) | bl;
}

// Avance d'un caractère UTF-8
static int nextChar(const String& s, int i) {
  i++;
  while (i < (int)s.length() && ((uint8_t)s[i] & 0xC0) == 0x80) i++;
  return i;
}

// Tronque `s` avec "..." pour tenir dans `w` pixels (police courante)
static String fitText(const String& s, int w) {
  if (canvas.textWidth(s) <= w) return s;
  int ell = canvas.textWidth("...");
  int end = 0;
  for (int i = 0; i < (int)s.length();) {
    int n = nextChar(s, i);
    if (canvas.textWidth(s.substring(0, n)) + ell > w) break;
    end = i = n;
  }
  return s.substring(0, end) + "...";
}

// Texte multi-lignes : coupe aux espaces, ou au caractère si un mot est trop
// long (chemins, commandes). Ajoute "..." si tout ne rentre pas.
// Renvoie le nombre de lignes dessinées.
static int drawWrapped(const String& text, int x, int y, int w, int maxLines, int lineH) {
  int line = 0;
  int pos = 0, len = text.length();
  while (pos <= len && line < maxLines) {
    int nl = text.indexOf('\n', pos);
    String para = text.substring(pos, nl < 0 ? len : nl);
    pos = nl < 0 ? len + 1 : nl + 1;
    para.replace("\t", "  ");

    while (line < maxLines) {
      // Plus long préfixe qui tient dans w, coupé de préférence à un espace
      int fit = 0, lastSpace = -1;
      for (int i = 0; i < (int)para.length();) {
        int n = nextChar(para, i);
        if (canvas.textWidth(para.substring(0, n)) > w) break;
        if (para[i] == ' ') lastSpace = i;
        fit = i = n;
      }
      int cut = fit;
      if (fit < (int)para.length() && lastSpace > 0) cut = lastSpace;

      String piece = para.substring(0, cut);
      String rest = para.substring(cut);
      rest.trim();
      bool more = rest.length() || pos <= len;
      if (line == maxLines - 1 && more) piece = fitText(piece + " ...", w);
      canvas.drawString(piece, x, y + line * lineH);
      line++;
      if (!rest.length()) break;
      para = rest;
    }
  }
  return line;
}

// --- icônes de la barre d'état

static void drawWifiIcon(int x, int y) {  // 14 x 12, ancré en bas à gauche
  bool on = WiFi.isConnected();
  int rssi = on ? WiFi.RSSI() : -100;
  int bars = !on ? 0 : rssi > -60 ? 3 : rssi > -72 ? 2 : 1;
  for (int i = 0; i < 3; i++) {
    int h = 4 + i * 4;
    canvas.fillRect(x + i * 5, y + 12 - h, 3, h, i < bars ? C_TEXT : C_DIM);
  }
  if (!on) canvas.drawLine(x, y, x + 13, y + 12, TFT_RED);
}

static void drawBattery(int x, int y) {  // 26 x 12
  int lvl = constrain(M5.Power.getBatteryLevel(), 0, 100);
  bool chg = M5.Power.isCharging();
  uint16_t c = chg ? (uint16_t)0x2629 : lvl > 30 ? C_TEXT : lvl > 15 ? (uint16_t)0xFBA2 : (uint16_t)TFT_RED;
  canvas.drawRoundRect(x, y, 23, 12, 2, C_MUTED);
  canvas.fillRect(x + 23, y + 4, 2, 4, C_MUTED);
  canvas.fillRect(x + 2, y + 2, (19 * lvl) / 100, 8, c);
  if (chg) {  // éclair
    canvas.fillTriangle(x + 13, y + 1, x + 8, y + 7, x + 12, y + 7, C_BG);
    canvas.fillTriangle(x + 10, y + 11, x + 15, y + 5, x + 11, y + 5, C_BG);
  }
}

static void drawStatusBar() {
  canvas.setFont(F_TEXT);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(C_MUTED);
  canvas.drawString(fitText(ui.project.length() ? ui.project : "Claude Pad", 230), 8, 12);

  drawWifiIcon(262, 6);
  drawBattery(286, 6);
  canvas.drawFastHLine(0, 24, 320, C_DIM);
}

// --- bandeau d'état : pastille animée + libellé

static void drawStateHeader(const Theme& t, uint32_t now) {
  bool waiting = ui.state == State::Waiting;
  uint16_t fill, fg;
  if (waiting) {
    // Pulsation douce entre orange vif et orange sombre
    float k = (sinf(now / 1000.0f * 2 * PI) + 1) / 2;
    fill = blend(t.accent, 0x6000, (uint8_t)(k * 140));
    fg = TFT_WHITE;
  } else {
    fill = blend(C_BG, t.accent, 60);
    fg = t.accent;
  }
  canvas.fillRoundRect(8, 30, 304, 40, 10, fill);

  // Indicateur à gauche
  int cx = 28, cy = 50;
  switch (ui.state) {
    case State::Working: {  // arc qui tourne
      int a = (now / 4) % 360;
      canvas.fillArc(cx, cy, 9, 6, a, a + 270, fg);
      break;
    }
    case State::Thinking:  // trois points qui respirent
      for (int i = 0; i < 3; i++) {
        float k = (sinf(now / 250.0f - i * 0.9f) + 1) / 2;
        canvas.fillCircle(cx - 8 + i * 8, cy, 2 + (int)(k * 2), fg);
      }
      break;
    case State::Waiting:
      canvas.fillCircle(cx, cy, 9, TFT_WHITE);
      canvas.setFont(F_BOLD);
      canvas.setTextDatum(middle_center);
      canvas.setTextColor(fill);
      canvas.drawString("!", cx + 1, cy + 1);
      break;
    case State::Done:  // coche
      canvas.drawWideLine(cx - 6, cy, cx - 2, cy + 5, 2, fg);
      canvas.drawWideLine(cx - 2, cy + 5, cx + 7, cy - 5, 2, fg);
      break;
    default:
      canvas.fillCircle(cx, cy, 5, fg);
  }

  canvas.setFont(F_BIG);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(waiting ? TFT_WHITE : C_TEXT);
  canvas.drawString(t.label, 46, cy + 1);
}

// --- menus de Claude : une ligne tactile par choix, « Non » en dernier

static bool menuOnScreen() { return ui.state == State::Waiting && ask.count > 0; }

static int rowH() { return ask.count >= 6 ? 23 : 27; }
static int rowY(int i) { return 236 - (ask.count - i) * rowH(); }

static void drawMenu(const Theme& t) {
  int top = rowY(0) - 5, h = rowH();
  canvas.setTextDatum(top_left);
  canvas.setFont(F_BOLD);
  canvas.setTextColor(C_TEXT);

  if (ask.kind == Ask::Permission) {
    canvas.drawString(fitText(ui.title, 300), 10, 78);
    if (ui.detail.length() && top - 99 >= 28) {
      canvas.fillRoundRect(8, 99, 304, top - 99, 6, C_CARD);
      canvas.fillRect(8, 104, 3, top - 109, t.accent);
      canvas.setFont(F_TEXT);
      drawWrapped(ui.detail, 18, 103, 288, (top - 103) / 18, 18);
    }
  } else {
    int y = 76 + 20 * drawWrapped(ui.title, 10, 76, 300, max(1, (top - 76) / 20), 20);
    if (ui.detail.length() && top - y >= 18) {
      canvas.setFont(F_TEXT);
      canvas.setTextColor(C_MUTED);
      drawWrapped(ui.detail, 10, y, 300, (top - y) / 18, 18);
    }
  }

  canvas.setFont(F_TEXT);
  for (int i = 0, n = 0; i < ask.count; i++) {
    const Choice& c = ask.choices[i];
    bool no = c.act == Act::Deny;
    uint16_t badge = no ? (uint16_t)0xE904
                   : c.act == Act::Toggle && !c.on ? C_DIM
                   : c.act == Act::Submit ? (uint16_t)0x2629 : t.accent;
    const char* mark = no ? "×" : c.act == Act::Submit ? "»" : nullptr;
    int ry = rowY(i);
    canvas.fillRoundRect(8, ry, 304, h - 4, 6, no ? blend(C_BG, 0xE904, 70) : C_CARD);
    canvas.fillRoundRect(12, ry + 3, 19, h - 10, 4, badge);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString(mark ? String(mark) : String(++n), 21, ry + h / 2 - 2);
    canvas.setTextDatum(middle_left);
    canvas.setTextColor(c.act == Act::Toggle && !c.on ? C_MUTED : C_TEXT);
    canvas.drawString(fitText(c.label, 266), 40, ry + h / 2 - 2);
  }
}

// --- contenu : titre + détail (dans une carte « code » quand c'est une commande)

// Coupe `text` en lignes de `w` pixels au plus (aux espaces de préférence)
static void wrapLines(const String& text, int w, std::vector<String>& out) {
  out.clear();
  int pos = 0, len = text.length();
  while (pos <= len) {
    int nl = text.indexOf('\n', pos);
    String para = text.substring(pos, nl < 0 ? len : nl);
    pos = nl < 0 ? len + 1 : nl + 1;
    if (!para.length()) { out.push_back(""); continue; }
    while (para.length()) {
      int fit = 0, lastSpace = -1;
      for (int i = 0; i < (int)para.length();) {
        int n = nextChar(para, i);
        if (canvas.textWidth(para.substring(0, n)) > w) break;
        if (para[i] == ' ') lastSpace = i;
        fit = i = n;
      }
      if (!fit) fit = nextChar(para, 0);
      int cut = fit < (int)para.length() && lastSpace > 0 ? lastSpace : fit;
      out.push_back(para.substring(0, cut));
      para = para.substring(cut);
      para.trim();
    }
  }
  while (out.size() && !out.back().length()) out.pop_back();
}

// Lignes de la réponse : la fin par défaut (la suite arrive en bas, comme un
// terminal), plus haut si on a fait glisser le doigt dessus
static void drawReply(int y, int maxLines) {
  canvas.setFont(F_TEXT);
  if (!reply.wrapped) {
    wrapLines(reply.text, 292, reply.lines);
    reply.wrapped = true;
  }
  int n = reply.lines.size();
  // Lecture en cours plus haut : les nouvelles lignes ne la décalent pas
  if (reply.scroll > 0) reply.scroll += n - reply.count;
  reply.count = n;
  reply.scroll = constrain(reply.scroll, 0, max(0, n - maxLines));

  int first = max(0, n - maxLines - reply.scroll);
  canvas.setTextDatum(top_left);
  for (int i = first; i < min(n, first + maxLines); i++) {
    const String& l = reply.lines[i];
    canvas.setTextColor(l == "[code]" ? C_MUTED : C_TEXT);
    canvas.drawString(l, 10, y + (i - first) * REPLY_LINE_H);
  }

  // Ascenseur à droite, seulement s'il y a de quoi défiler
  if (n > maxLines) {
    int h = maxLines * REPLY_LINE_H;
    int barH = max(12, h * maxLines / n);
    int barY = y + (h - barH) * first / (n - maxLines);
    canvas.fillRoundRect(314, barY, 3, barH, 1, reply.scroll ? C_TEXT : C_DIM);
  }
}

// Ajoute un paquet de lignes à la réponse : enlève le Markdown, remplace les
// blocs de code par « [code] », ne garde que la fin.
static void appendReply(const String& msgId, const String& delta) {
  if (msgId != reply.msgId) {
    reply.msgId = msgId;
    reply.text = "";
    reply.inFence = false;
    reply.scroll = reply.count = 0;
  }
  int pos = 0, len = delta.length();
  while (pos < len) {
    int nl = delta.indexOf('\n', pos);
    String line = delta.substring(pos, nl < 0 ? len : nl);
    pos = nl < 0 ? len : nl + 1;
    String t = line;
    t.trim();
    if (t.startsWith("```")) {
      if (!reply.inFence) reply.text += "[code]\n";
      reply.inFence = !reply.inFence;
      continue;
    }
    if (reply.inFence || t.startsWith("|-") || t.startsWith("| -")) continue;
    while (t.startsWith("#")) t.remove(0, 1);
    if (t.startsWith("- ") || t.startsWith("* ")) t = "• " + t.substring(2);
    t.replace("**", "");
    t.replace("`", "");
    t.trim();
    reply.text += t + (nl < 0 ? "" : "\n");
  }
  if ((int)reply.text.length() > REPLY_MAX) {
    int cut = reply.text.indexOf('\n', reply.text.length() - REPLY_MAX);
    reply.text = reply.text.substring(cut < 0 ? reply.text.length() - REPLY_MAX : cut + 1);
  }
  reply.wrapped = false;
  ui.dirty = true;
}

static bool replyOnScreen() {
  return view == View::Live && ui.state != State::Waiting && reply.text.length();
}

static void drawBody(const Theme& t) {
  if (menuOnScreen()) return drawMenu(t);

  canvas.setTextDatum(top_left);
  canvas.setFont(F_BOLD);
  canvas.setTextColor(C_TEXT);
  canvas.drawString(fitText(ui.title, 300), 10, 78);

  // Hors demande : la réponse de Claude plutôt que le détail des outils
  if (replyOnScreen()) return drawReply(REPLY_Y, REPLY_LINES);
  if (!ui.detail.length()) return;
  bool code = ui.state == State::Working || ui.state == State::Waiting;
  canvas.setFont(F_TEXT);
  if (code) {
    canvas.fillRoundRect(8, 99, 304, 130, 6, C_CARD);
    canvas.fillRect(8, 105, 3, 118, t.accent);
    canvas.setTextColor(C_TEXT);
    drawWrapped(ui.detail, 18, 104, 288, 6, 20);
  } else {
    canvas.setTextColor(C_MUTED);
    drawWrapped(ui.detail, 10, 102, 300, 6, 20);
  }
}

// --- vues Sessions et Conso

static Session* findSession(const String& id) {
  for (Session& s : sessions)
    if (s.id.length() && s.id == id) return &s;
  return nullptr;
}

// Sessions encore fraîches, de la plus récente à la plus ancienne
static int sortedSessions(Session** out) {
  int n = 0;
  uint32_t now = millis();
  for (Session& s : sessions)
    if (s.id.length() && now - s.seen < SESSION_STALE_MS) out[n++] = &s;
  std::sort(out, out + n, [](Session* a, Session* b) { return a->seen > b->seen; });
  return n;
}

static String ago(uint32_t ms) {
  uint32_t s = ms / 1000;
  if (s < 60) return "à l'instant";
  if (s < 3600) return String(s / 60) + " min";
  return String(s / 3600) + " h";
}

// 950 / 12,3 k / 142 k / 1,23 M
static String fmtTokens(uint64_t n) {
  char buf[16];
  if (n < 1000) snprintf(buf, sizeof(buf), "%u", (unsigned)n);
  else if (n < 10000) snprintf(buf, sizeof(buf), "%.1f k", n / 1e3);
  else if (n < 1000000) snprintf(buf, sizeof(buf), "%u k", (unsigned)(n / 1000));
  else snprintf(buf, sizeof(buf), "%.2f M", n / 1e6);
  String s(buf);
  s.replace(".", ",");
  return s;
}

static String fmtCost(float c) {
  char buf[16];
  snprintf(buf, sizeof(buf), c < 10 ? "%.2f $" : "%.1f $", c);
  String s(buf);
  s.replace(".", ",");
  return s;
}

static void drawViewHeader(const char* label, const String& right) {
  canvas.fillRoundRect(8, 30, 304, 40, 10, blend(C_BG, C_MUTED, 45));
  canvas.setFont(F_BIG);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(C_TEXT);
  canvas.drawString(label, 18, 51);
  canvas.setFont(F_TEXT);
  canvas.setTextDatum(middle_right);
  canvas.setTextColor(C_MUTED);
  canvas.drawString(fitText(right, 130), 302, 51);
}

constexpr int SESSION_ROW_Y = 78, SESSION_ROW_H = 30, SESSION_ROWS = 5;

static void drawSessions() {
  Session* list[MAX_SESSIONS];
  int n = sortedSessions(list);
  drawViewHeader("SESSIONS", n ? String(n) + " en cours" : String());

  canvas.setFont(F_TEXT);
  if (!n) {
    canvas.setTextDatum(top_left);
    canvas.setTextColor(C_MUTED);
    drawWrapped("Aucune session vue depuis le démarrage du pad.", 10, 82, 300, 3, 20);
    return;
  }
  uint32_t now = millis();
  for (int i = 0; i < n && i < SESSION_ROWS; i++) {
    const Session& s = *list[i];
    Theme t = themeFor(s.state);
    int y = SESSION_ROW_Y + i * SESSION_ROW_H;
    canvas.fillRoundRect(8, y, 304, SESSION_ROW_H - 4, 6, C_CARD);
    if (s.state == State::Waiting) canvas.drawRoundRect(8, y, 304, SESSION_ROW_H - 4, 6, t.accent);
    canvas.fillCircle(21, y + 13, 5, t.accent);

    String right = String(t.label) + " · " + ago(now - s.seen);
    canvas.setTextDatum(middle_right);
    canvas.setTextColor(C_MUTED);
    canvas.drawString(right, 302, y + 12);
    int rw = canvas.textWidth(right);

    canvas.setFont(F_BOLD);
    canvas.setTextDatum(middle_left);
    canvas.setTextColor(C_TEXT);
    canvas.drawString(fitText(s.project.length() ? s.project : "?", 300 - 34 - rw - 8), 34, y + 12);
    canvas.setFont(F_TEXT);
  }
  if (n > SESSION_ROWS) {
    canvas.setTextDatum(top_left);
    canvas.setTextColor(C_MUTED);
    canvas.drawString("+ " + String(n - SESSION_ROWS) + " autres", 10, SESSION_ROW_Y + SESSION_ROWS * SESSION_ROW_H);
  }
}

static void drawUsage() {
  Session* s = findSession(focusId.length() ? focusId : lastSessionId);
  drawViewHeader("CONSO", s ? s->project : String());
  canvas.setFont(F_TEXT);
  canvas.setTextDatum(top_left);

  if (!s || !s->hasUsage) {
    canvas.setTextColor(C_MUTED);
    drawWrapped("Pas encore de données. Elles arrivent après chaque outil et à la fin de chaque réponse.",
                10, 82, 300, 5, 20);
    return;
  }

  // Contexte : jauge verte / orange / rouge
  float k = s->window ? (float)s->context / s->window : 0;
  uint16_t gauge = k < 0.5f ? (uint16_t)0x2629 : k < 0.8f ? (uint16_t)0xFBA2 : (uint16_t)0xE904;
  canvas.setTextColor(C_TEXT);
  canvas.drawString("Contexte", 10, 78);
  canvas.setTextDatum(top_right);
  canvas.setTextColor(C_MUTED);
  canvas.drawString(fmtTokens(s->context) + " / " + fmtTokens(s->window) + "  " +
                    String((int)(k * 100 + 0.5f)) + " %", 310, 78);
  canvas.fillRoundRect(10, 99, 300, 10, 5, C_CARD);
  if (k > 0) canvas.fillRoundRect(10, 99, max(10, (int)(300 * min(k, 1.0f))), 10, 5, gauge);

  // Tokens cumulés, en 2 x 2
  struct { const char* label; uint64_t n; } cells[] = {
    {"Entrée", s->input}, {"Sortie", s->output},
    {"Cache lu", s->cacheRead}, {"Cache écrit", s->cacheWrite},
  };
  for (int i = 0; i < 4; i++) {
    int x = 8 + (i % 2) * 154, y = 117 + (i / 2) * 28;
    canvas.fillRoundRect(x, y, 150, 24, 6, C_CARD);
    canvas.setTextDatum(middle_left);
    canvas.setTextColor(C_MUTED);
    canvas.drawString(cells[i].label, x + 8, y + 12);
    canvas.setTextDatum(middle_right);
    canvas.setTextColor(C_TEXT);
    canvas.drawString(fmtTokens(cells[i].n), x + 142, y + 12);
  }

  // Prix de la session + total de toutes les sessions
  canvas.setFont(F_BIG);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(C_TEXT);
  canvas.drawString(fmtCost(s->cost), 10, 189);
  canvas.setFont(F_TEXT);
  canvas.setTextDatum(middle_right);
  canvas.setTextColor(C_MUTED);
  canvas.drawString(s->model + " · prix API", 310, 189);

  float total = 0;
  int n = 0;
  for (const Session& o : sessions)
    if (o.id.length() && o.hasUsage) { total += o.cost; n++; }
  if (n > 1) {
    canvas.setTextDatum(middle_left);
    canvas.drawString("Toutes les sessions : " + fmtCost(total), 10, 216);
  }
}

// Trois repères au bas de l'écran, au-dessus des zones A / B / C
static void drawViewDots() {
  const View order[] = {View::Sessions, View::Usage, View::Live};
  for (int i = 0; i < 3; i++)
    canvas.fillRoundRect(38 + i * 107, 235, 30, 4, 2, order[i] == view ? C_TEXT : C_DIM);
}

static void render() {
  uint32_t now = millis();
  Theme t = themeFor(ui.state);
  canvas.fillSprite(C_BG);
  drawStatusBar();
  if (view == View::Sessions) {
    drawSessions();
  } else if (view == View::Usage) {
    drawUsage();
  } else {
    drawStateHeader(t, now);
    drawBody(t);
  }
  if (!menuOnScreen()) drawViewDots();
  canvas.pushSprite(0, 0);
  ui.dirty = false;
}

// Veille : écran atténué après 5 min sans événement (hors attente)
uint32_t lastActivity = 0;
bool dimmed = false;

static void wake() {
  lastActivity = millis();
  if (dimmed) {
    dimmed = false;
    M5.Display.setBrightness(120);
  }
}

// ------------------------------------------------------- serveur HTTP ----

static bool isEditTool(const String& t) {
  return t == "Edit" || t == "Write" || t == "MultiEdit" || t == "NotebookEdit";
}

// Affiche la question `ask.qIndex` d'un AskUserQuestion
static void loadQuestion() {
  JsonObjectConst q = ask.questions[ask.qIndex];
  bool multi = q["multiSelect"] | false;
  String detail = q["header"] | "";
  if (ask.qCount > 1) detail += "  (" + String(ask.qIndex + 1) + "/" + String(ask.qCount) + ")";
  if (multi) detail += "  · plusieurs choix";

  ask.count = 0;
  for (JsonObjectConst o : q["options"].as<JsonArrayConst>()) {
    if (ask.count == 4) break;
    ask.choices[ask.count++] = {o["label"] | "?", multi ? Act::Toggle : Act::Pick};
  }
  if (multi) ask.choices[ask.count++] = {"Valider", Act::Submit};
  ask.choices[ask.count++] = {"Annuler", Act::Deny};
  setState(State::Waiting, q["question"] | "Question", detail);
}

// Claude ouvre un menu : permission, question ou validation de plan
static void openPrompt(JsonDocument& doc, const String& id) {
  String tool = doc["tool_name"] | "outil";
  JsonVariantConst in = doc["tool_input"];
  ask.count = 0;

  if (tool == "AskUserQuestion") {
    ask.kind = Ask::Question;
    ask.questions.set(in["questions"]);
    ask.qCount = min((int)ask.questions.size(), 4);
    ask.qIndex = 0;
    loadQuestion();
  } else if (tool == "ExitPlanMode") {
    ask.kind = Ask::Plan;
    ask.choices[ask.count++] = {"Oui, lancer le plan", Act::Allow};
    ask.choices[ask.count++] = {"Non, continuer à planifier", Act::Deny};
    String plan = in["plan"] | "";
    plan.replace("#", "");
    plan.trim();
    setState(State::Waiting, "Valider le plan ?", plan);
  } else {
    ask.kind = Ask::Permission;
    lastTool = tool;
    lastWhat = firstOf(in, {"command", "file_path", "pattern", "url", "description"});
    ask.choices[ask.count++] = {"Oui", Act::Allow};
    // « Toujours » : on renvoie la règle que Claude propose lui-même
    if (doc["permission_suggestions"].size() > 0 || isEditTool(tool))
      ask.choices[ask.count++] = {"Oui, ne plus demander", Act::Always};
    ask.choices[ask.count++] = {"Non", Act::Deny};
    setState(State::Waiting, "Autoriser " + tool + " ?", lastWhat);
  }
  ask.id = id;  // après setState, qui l'efface hors attente
  alertWaiting();
}

// Session du hook (créée au besoin, en recyclant la plus ancienne)
static Session* sessionFor(JsonDocument& doc) {
  String id = doc["session_id"] | "";
  if (!id.length()) return nullptr;
  Session* s = findSession(id);
  if (!s) {
    s = &sessions[0];
    for (Session& o : sessions) {
      if (!o.id.length()) { s = &o; break; }
      if (o.seen < s->seen) s = &o;
    }
    *s = Session();
    s->id = id;
  }
  String cwd = doc["cwd"] | "";
  if (cwd.length()) s->project = basename(cwd);
  s->seen = millis();
  lastSessionId = id;
  if (view != View::Live) ui.dirty = true;
  return s;
}

// État d'une session d'après l'événement reçu (indépendant de l'affichage
// direct, qui garde une demande à l'écran tant qu'elle n'est pas close)
static void trackSession(JsonDocument& doc, const String& ev, const String& tool) {
  if (ev == "SessionEnd") {
    Session* s = findSession(doc["session_id"] | "");
    if (s) *s = Session();
    ui.dirty = true;
    return;
  }
  Session* s = sessionFor(doc);
  if (!s) return;
  if (ev == "SessionStart") s->state = State::Idle;
  else if (ev == "UserPromptSubmit" || ev == "PostToolUse" || ev == "MessageDisplay") s->state = State::Thinking;
  else if (ev == "PermissionRequest") s->state = State::Waiting;
  else if (ev == "PreToolUse")
    s->state = tool == "AskUserQuestion" || tool == "ExitPlanMode" ? State::Waiting : State::Working;
  else if (ev == "Notification")
    s->state = String(doc["notification_type"] | "") == "idle_prompt" ? State::Done : State::Waiting;
  else if (ev == "Stop") s->state = State::Done;
}

// POST /usage : envoyé par pad-usage.py
static void handleUsage() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "bad json");
    return;
  }
  server.send(200);
  Session* s = findSession(doc["session_id"] | "");
  if (!s) s = sessionFor(doc);  // session déjà connue : on ne touche pas à `seen`
  if (!s) return;
  s->hasUsage = true;
  s->model = doc["model"] | "";
  s->context = doc["context"] | 0;
  s->window = doc["window"] | 0;
  s->input = doc["input"] | (uint64_t)0;
  s->output = doc["output"] | (uint64_t)0;
  s->cacheRead = doc["cache_read"] | (uint64_t)0;
  s->cacheWrite = doc["cache_write"] | (uint64_t)0;
  s->cost = doc["cost"] | 0.0f;
  if (view == View::Usage) ui.dirty = true;
}

// Décision au format attendu par Claude Code (sortie JSON d'un hook)
static String hookOutput(const char* decision) {
  JsonDocument out;
  JsonObject h = out["hookSpecificOutput"].to<JsonObject>();
  String ev = held.event["hook_event_name"] | "";
  bool deny = strcmp(decision, "deny") == 0;
  h["hookEventName"] = ev;
  if (ev == "PermissionRequest") {
    JsonObject d = h["decision"].to<JsonObject>();
    d["behavior"] = deny ? "deny" : "allow";
    if (deny) d["message"] = DENY_REASON;
    if (strcmp(decision, "always") == 0) {
      if (held.event["permission_suggestions"].size() > 0) {
        d["updatedPermissions"] = held.event["permission_suggestions"];
      } else if (isEditTool(held.event["tool_name"] | "")) {
        // Pas de suggestion pour les modifs de fichiers : on accepte les
        // modifications pour le reste de la session
        JsonObject r = d["updatedPermissions"].to<JsonArray>().add<JsonObject>();
        r["type"] = "setMode";
        r["mode"] = "acceptEdits";
        r["destination"] = "session";
      }
    }
  } else {  // PreToolUse : AskUserQuestion, ExitPlanMode
    h["permissionDecision"] = deny ? "deny" : "allow";
    if (deny) {
      h["permissionDecisionReason"] = DENY_REASON;
    } else {
      h["updatedInput"] = held.event["tool_input"];
      if (ask.kind == Ask::Question) {
        JsonObject a = h["updatedInput"]["answers"].to<JsonObject>();
        for (int i = 0; i < ask.qCount; i++) a[String(ask.questions[i]["question"] | "")] = ask.answers[i];
      }
    }
  }
  String json;
  serializeJson(out, json);
  return json;
}

// Répond au hook en attente et ferme la connexion. Corps vide : Claude Code
// affiche son menu habituel.
static void finishHeld(const String& body) {
  if (!held.client) return;
  held.client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
                    "Content-Length: " + String(body.length()) + "\r\n\r\n" + body);
  held.client.stop();
  held.client = WiFiClient();
  held.event.clear();
}

static void resolve(const char* decision, const String& summary) {
  finishHeld(hookOutput(decision));
  buzz(40);
  if (strcmp(decision, "deny") == 0) setState(State::Idle, "Refusé");
  else setState(State::Working, ask.kind == Ask::Permission ? "Accepté" : "Réponse envoyée", summary);
}

// POST /hook : PermissionRequest, et PreToolUse pour AskUserQuestion /
// ExitPlanMode. Pas de réponse ici : elle part au choix touché (resolve),
// ou vide à l'abandon (délai, réponse donnée sur le PC).
static void handleHook() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "bad json");
    return;
  }
  String ev = doc["hook_event_name"] | "";
  String tool = doc["tool_name"] | "";
  bool interactive = tool == "AskUserQuestion" || tool == "ExitPlanMode";
  // Questions et plan passent par PreToolUse ; s'ils arrivent aussi en
  // PermissionRequest (délai dépassé...), on laisse le menu du PC.
  if (!(ev == "PermissionRequest" && !interactive) && !(ev == "PreToolUse" && interactive)) {
    server.send(200);
    return;
  }

  finishHeld("");  // une seule demande à la fois : la précédente retourne au PC
  wake();
  String cwd = doc["cwd"] | "";
  if (cwd.length()) ui.project = basename(cwd);
  Session* s = sessionFor(doc);
  if (s) s->state = State::Waiting;
  openPrompt(doc, String(++promptSeq));
  held.client = server.client();  // garde la connexion ouverte
  held.event = doc;
  held.since = millis();
}

// Demande en attente abandonnée par Claude Code (réponse donnée sur le PC,
// session arrêtée) ou trop longue : retour au menu du PC
static void checkHeld() {
  if (!held.client) return;
  static uint32_t last = 0;
  if (millis() - last < 300) return;
  last = millis();
  bool gone = !held.client.connected();
  bool late = millis() - held.since > PROMPT_TIMEOUT_MS;
  if (!gone && !late) return;
  finishHeld("");
  if (ui.state == State::Waiting && ask.id.length())
    setState(State::Idle, late ? "Délai dépassé" : "Répondu sur le PC", late ? "Réponds sur le PC." : "");
}

static void handleEvent() {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "text/plain", String("bad json: ") + err.c_str());
    return;
  }
  server.send(200);  // corps vide : succès pour un hook http

  String ev = doc["hook_event_name"] | "";
  String tool = doc["tool_name"] | "";
  trackSession(doc, ev, tool);
  if (ev == "SessionEnd") return;

  // Réponse de Claude, ligne par ligne
  if (ev == "MessageDisplay") {
    appendReply(doc["message_id"] | "", doc["delta"] | "");
    wake();
    if (ui.state != State::Waiting) {
      String cwd = doc["cwd"] | "";
      if (cwd.length()) ui.project = basename(cwd);
      setState(State::Thinking, "Claude répond");
    }
    return;
  }

  // Les demandes passent par /hook, pas par ici
  if (ev == "PermissionRequest") return;
  if (ev == "PreToolUse" && (tool == "AskUserQuestion" || tool == "ExitPlanMode")) return;

  // Une demande est à l'écran : on ne la remplace que si elle est close
  // (outil terminé, réponse finie, nouveau message), pas pour l'activité
  // d'un autre outil ou d'un sous-agent en parallèle.
  if (ui.state == State::Waiting && ask.id.length()) {
    bool closes = ev == "Stop" || ev == "UserPromptSubmit" || ev == "SessionStart" ||
                  (ev == "PostToolUse" && ask.kind == Ask::Permission && tool == lastTool) ||
                  (ev == "PostToolUse" && ask.kind == Ask::Question && tool == "AskUserQuestion") ||
                  (ev == "PostToolUse" && ask.kind == Ask::Plan && tool == "ExitPlanMode");
    if (!closes) {
      if (ev == "Notification") buzz(200);  // rappel
      return;
    }
  }

  wake();
  String cwd = doc["cwd"] | "";
  if (cwd.length()) ui.project = basename(cwd);

  if (ev == "SessionStart") {
    setState(State::Idle, "Session démarrée");
  } else if (ev == "UserPromptSubmit") {
    String prompt = doc["prompt"] | "";
    reply = Reply();  // nouveau tour : la réponse précédente s'efface
    setState(State::Thinking, "Claude réfléchit...", prompt);
  } else if (ev == "PostToolUse") {
    setState(State::Thinking, "Claude réfléchit...");
  } else if (ev == "PreToolUse") {
    lastTool = tool;
    lastWhat = firstOf(doc["tool_input"], {"command", "file_path", "pattern", "url", "description"});
    setState(State::Working, tool);  // pas de code à l'écran : la réponse reste affichée
  } else if (ev == "Notification") {
    String type = doc["notification_type"] | "";
    String msg = doc["message"] | "Claude a besoin de toi";
    if (type == "idle_prompt") {
      setState(State::Done, "Claude attend ton message", msg);
      buzz(120);
    } else {
      // Demande sans hook /hook (ou délai dépassé) : réponse sur le PC
      ask.kind = Ask::Other;
      setState(State::Waiting, "Réponds sur le PC", msg);
      alertWaiting();
    }
  } else if (ev == "Stop" || ev == "SubagentStop") {
    setState(State::Done, ev == "Stop" ? "Réponse terminée" : "Sous-agent terminé");
    buzz(120);
  }
}

static void handleRoot() {
  JsonDocument doc;
  doc["state"] = themeFor(ui.state).label;
  doc["project"] = ui.project;
  doc["title"] = ui.title;
  doc["prompt"] = ask.id;
  doc["battery"] = M5.Power.getBatteryLevel();
  Session* list[MAX_SESSIONS];
  int n = sortedSessions(list);
  JsonArray arr = doc["sessions"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["project"] = list[i]->project;
    o["state"] = themeFor(list[i]->state).label;
    if (list[i]->hasUsage) {
      o["context"] = list[i]->context;
      o["cost"] = list[i]->cost;
    }
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ------------------------------------------------------------ tactile ----

// Choix i du menu à l'écran
static void choose(int i) {
  Choice& c = ask.choices[i];
  switch (c.act) {
    case Act::Allow:  return resolve("allow", c.label);
    case Act::Always: return resolve("always", c.label);
    case Act::Deny:   return resolve("deny", c.label);
    case Act::Toggle:
      c.on = !c.on;
      buzz(20);
      ui.dirty = true;
      return;
    case Act::Pick:
    case Act::Submit: {
      // Réponse à la question en cours (cases cochées jointes par des virgules)
      String a;
      if (c.act == Act::Pick) a = c.label;
      else
        for (int k = 0; k < ask.count; k++)
          if (ask.choices[k].act == Act::Toggle && ask.choices[k].on)
            a += (a.length() ? ", " : "") + ask.choices[k].label;
      if (!a.length()) { buzz(60); return; }  // rien de coché
      ask.answers[ask.qIndex] = a;
      if (++ask.qIndex < ask.qCount) {
        buzz(20);
        String id = ask.id;
        loadQuestion();
        ask.id = id;
        return;
      }
      return resolve("allow", a);
    }
  }
}

static void showView(View v) {
  view = view == v ? View::Live : v;  // même zone une 2e fois : retour au direct
  if (view == View::Usage && v == View::Usage) focusId = "";  // session la plus récente
  viewSince = millis();
  ui.dirty = true;
  buzz(15);
}

// Les choix se font sur l'écran. Zones sous l'écran (y >= 240) :
// A = Sessions, B = Conso, C = Direct, inactives quand un menu est affiché.
static void handleTouch() {
  auto touch = M5.Touch.getDetail();

  // Glisser le doigt sur la réponse : la fait défiler (vers le bas = plus ancien)
  static int drag = 0;
  if (touch.isDragging() && !dimmed && replyOnScreen() && touch.y >= REPLY_Y - 4 && touch.y < 240) {
    drag += touch.deltaY();
    int lines = drag / REPLY_LINE_H;
    if (lines) {
      drag -= lines * REPLY_LINE_H;
      int before = reply.scroll;
      reply.scroll = max(0, reply.scroll + lines);  // plafonné au dessin
      if (reply.scroll != before) ui.dirty = true;
    }
    lastActivity = millis();
    return;
  }
  if (!touch.isPressed()) drag = 0;
  if (!touch.wasPressed()) return;

  // Écran en veille : le premier appui ne fait que le réveiller
  if (dimmed) { wake(); ui.dirty = true; return; }
  lastActivity = millis();

  if (touch.y >= 240) {
    if (menuOnScreen()) return;
    const View zones[] = {View::Sessions, View::Usage, View::Live};
    return showView(zones[constrain(touch.x / 107, 0, 2)]);
  }

  // Liste des sessions : une ligne touchée ouvre sa conso
  if (view == View::Sessions) {
    Session* list[MAX_SESSIONS];
    int n = sortedSessions(list);
    int i = (touch.y - SESSION_ROW_Y) / SESSION_ROW_H;
    if (touch.y >= SESSION_ROW_Y && i < n && i < SESSION_ROWS) {
      focusId = list[i]->id;
      view = View::Usage;
      viewSince = millis();
      ui.dirty = true;
      buzz(15);
    }
    return;
  }
  if (view != View::Live) { viewSince = millis(); return; }

  if (!menuOnScreen() || !ask.id.length()) return;

  for (int i = 0; i < ask.count; i++)
    if (touch.y >= rowY(i) && touch.y < rowY(i) + rowH() - 4) return choose(i);
}

// --------------------------------------------------------------- Wi-Fi ----

// Identifiants enregistrés en NVS par la page de réglage
Preferences prefs;
DNSServer dns;
bool portal = false;      // point d'accès de réglage ouvert
uint32_t portalSince = 0;
String savedSsid;         // réseau enregistré (vide : aucun)
String portalNets;        // boutons des réseaux trouvés
constexpr uint32_t PORTAL_RETRY_MS = 5UL * 60 * 1000;  // box en panne : on réessaie

static void loadWifi(String& ssid, String& pass) {
  prefs.begin("wifi", true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
#ifdef WIFI_SSID
  if (!ssid.length()) { ssid = WIFI_SSID; pass = WIFI_PASSWORD; }
#endif
}

static String htmlEscape(const String& in) {
  String out;
  for (char c : in) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

static void scanNetworks() {
  int n = WiFi.scanNetworks();
  portalNets = "";
  for (int i = 0; i < n; i++) {
    String name = htmlEscape(WiFi.SSID(i));
    if (!name.length() || portalNets.indexOf(">" + name + "<") >= 0) continue;  // plusieurs bornes
    portalNets += "<button type=button onclick=\"pick(this)\">" + name + "</button>";
  }
  WiFi.scanDelete();
}

static void drawPortal() {
  canvas.fillSprite(C_BG);
  canvas.setFont(F_BOLD);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(C_TEXT);
  canvas.drawString("Réglage du Wi-Fi", 12, 18);
  canvas.drawFastHLine(0, 34, 320, C_DIM);

  // QR code « rejoindre le réseau » (appareil photo du téléphone)
  canvas.fillRoundRect(10, 50, 128, 128, 6, TFT_WHITE);
  canvas.qrcode((String("WIFI:T:nopass;S:") + AP_NAME + ";;").c_str(), 16, 56, 116);

  canvas.setFont(F_TEXT);
  canvas.setTextColor(C_MUTED);
  canvas.drawString("1. Rejoins le Wi-Fi", 150, 56);
  canvas.setFont(F_BIG);
  canvas.setTextColor(0xFBA2);
  canvas.drawString(AP_NAME, 150, 84);
  canvas.setFont(F_TEXT);
  canvas.setTextColor(C_MUTED);
  canvas.drawString("(ou scanne le code)", 150, 110);
  canvas.drawString("2. Choisis ton réseau", 150, 142);
  canvas.drawString("sur la page qui s'ouvre", 150, 162);
  canvas.setTextColor(C_TEXT);
  canvas.drawString("http://192.168.4.1", 150, 182);

  if (savedSsid.length()) {
    canvas.setTextColor(C_MUTED);
    canvas.setTextDatum(middle_center);
    canvas.drawString(fitText("« " + savedSsid + " » ne répond pas", 300), 160, 210);
    canvas.drawString("nouvel essai dans 5 min", 160, 228);
  }
  canvas.pushSprite(0, 0);
}

static const char PORTAL_HEAD[] PROGMEM = R"(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Claude Pad</title>
<style>body{font:16px system-ui,sans-serif;background:#0b0b0f;color:#e8ecec;margin:0;padding:20px;max-width:420px}
h1{font-size:22px}p{color:#8a8d8f}input,button{font:inherit;box-sizing:border-box;width:100%;padding:12px;margin:6px 0;border-radius:8px;border:1px solid #3a3c3c;background:#1c1d24;color:inherit;text-align:left}
button[type=submit]{background:#f97316;border:0;color:#fff;font-weight:600;text-align:center}a{color:#f97316}</style>
<h1>Claude Pad</h1>)";

static void handlePortal() {
  portalSince = millis();
  if (server.hasArg("scan")) scanNetworks();
  String html = FPSTR(PORTAL_HEAD);
  html += "<p>Choisis le réseau Wi-Fi du pad (2,4 GHz), puis son mot de passe.</p>";
  html += portalNets.length() ? portalNets : String("<p>Aucun réseau trouvé.</p>");
  html += "<p><a href=\"/?scan=1\">Rechercher à nouveau</a></p>"
          "<form method=post action=/save><input name=ssid id=ssid placeholder=\"Nom du réseau\" required value=\"" +
          htmlEscape(savedSsid) + "\"><input name=pass type=password placeholder=\"Mot de passe\">"
          "<button type=submit>Enregistrer et redémarrer</button></form>"
          "<script>function pick(b){ssid.value=b.textContent;document.querySelector('[name=pass]').focus()}</script>";
  server.send(200, "text/html; charset=utf-8", html);
}

static void handleSave() {
  String ssid = server.arg("ssid");
  ssid.trim();
  if (!ssid.length()) return server.send(400, "text/plain; charset=utf-8", "Nom du réseau manquant");
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", server.arg("pass"));
  prefs.end();
  String html = FPSTR(PORTAL_HEAD);
  html += "<p>Enregistré. Le pad redémarre et rejoint « " + htmlEscape(ssid) +
          " ». Son adresse s'affiche sur l'écran.</p>";
  server.send(200, "text/html; charset=utf-8", html);
  delay(800);
  ESP.restart();
}

// Point d'accès ouvert + DNS qui renvoie tout vers le pad : le téléphone
// affiche la page de réglage tout seul.
static void startPortal() {
  portal = true;
  portalSince = millis();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);  // STA pour lister les réseaux
  scanNetworks();
  WiFi.softAP(AP_NAME);
  dns.start(53, "*", WiFi.softAPIP());
  server.on("/", HTTP_GET, handlePortal);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound([] {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", "");
  });
  server.begin();
  Serial.printf("[wifi] portail ouvert : %s -> http://192.168.4.1\n", AP_NAME);
  drawPortal();
}

// --------------------------------------------------------------- setup ----

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(120);
  M5.Speaker.setVolume(140);

  canvas.setColorDepth(16);
  canvas.setPsram(true);
  canvas.createSprite(320, 240);

  canvas.fillSprite(C_BG);
  canvas.setFont(F_BIG);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(C_TEXT);
  canvas.drawString("Claude Pad", 160, 100);
  canvas.setFont(F_TEXT);
  canvas.setTextColor(C_MUTED);
  String ssid, pass;
  loadWifi(ssid, pass);
  savedSsid = ssid;
  canvas.drawString(ssid.length() ? "Connexion à " + ssid + "..." : String("Aucun Wi-Fi enregistré"), 160, 136);
  if (ssid.length()) canvas.drawString("Toucher l'écran : régler le Wi-Fi", 160, 200);
  canvas.pushSprite(0, 0);

  bool force = false;
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(PAD_HOSTNAME);
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
      if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
        Serial.printf("[wifi] deconnecte, raison=%d\n", info.wifi_sta_disconnected.reason);
      else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
        Serial.printf("[wifi] connecte, IP=%s RSSI=%d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    });
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (!WiFi.isConnected() && millis() - t0 < 20000) {
      M5.update();
      if (M5.Touch.getCount()) { force = true; break; }
      delay(50);
    }
  }
  if (force || !WiFi.isConnected()) {
    if (force) savedSsid = "";  // demandé : pas de message « ne répond pas »
    return startPortal();
  }

  MDNS.begin(PAD_HOSTNAME);
  MDNS.addService("http", "tcp", 80);
  Serial.printf("IP: %s  ->  http://%s.local\n", WiFi.localIP().toString().c_str(), PAD_HOSTNAME);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/event", HTTP_POST, handleEvent);
  server.on("/hook", HTTP_POST, handleHook);
  server.on("/usage", HTTP_POST, handleUsage);
  server.begin();

  setState(State::Idle, WiFi.localIP().toString(), String("http://") + PAD_HOSTNAME + ".local");
  lastActivity = millis();
}

// ---------------------------------------------------------------- loop ----

void loop() {
  if (portal) {
    dns.processNextRequest();
    server.handleClient();
    // Réseau enregistré mais injoignable (box en panne ?) : on réessaie
    if (savedSsid.length() && millis() - portalSince > PORTAL_RETRY_MS) ESP.restart();
    delay(5);
    return;
  }

  M5.update();
  server.handleClient();
  checkHeld();
  handleTouch();

  uint32_t now = millis();

  if (vibrateUntil && now > vibrateUntil) {
    M5.Power.setVibration(0);
    vibrateUntil = 0;
  }

  // Rappel toutes les 30 s tant que Claude attend
  static uint32_t lastReminder = 0;
  if (ui.state == State::Waiting && now - ui.since > 30000 && now - lastReminder > 30000) {
    lastReminder = now;
    buzz(200);
  }

  static uint32_t lastRssiLog = 0;
  if (now - lastRssiLog > 5000) {
    lastRssiLog = now;
    Serial.printf("[wifi] %s RSSI=%d dBm\n", WiFi.isConnected() ? "ok" : "KO", WiFi.RSSI());
  }

  // Changement de connectivité -> redessine
  bool wifi = WiFi.isConnected();
  if (wifi != lastWifiConnected) {
    lastWifiConnected = wifi;
    ui.dirty = true;
  }

  // Veille de l'écran (jamais quand Claude attend une réponse)
  if (ui.state == State::Waiting) {
    wake();
  } else if (!dimmed && now - lastActivity > DIM_AFTER_MS) {
    dimmed = true;
    M5.Display.setBrightness(15);
  }

  // Sessions / Conso : retour au direct après 30 s sans appui
  if (view != View::Live && now - viewSince > VIEW_TIMEOUT_MS) {
    view = View::Live;
    ui.dirty = true;
  }

  // ~15 images/s pour les animations, 1/s sinon (batterie, Wi-Fi, âge des sessions)
  static uint32_t lastFrame = 0;
  uint32_t period = view == View::Live && animated(ui.state) && !dimmed ? 66 : 1000;
  if (ui.dirty || now - lastFrame >= period) {
    lastFrame = now;
    render();
  }

  delay(5);
}

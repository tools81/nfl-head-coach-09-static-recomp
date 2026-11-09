# Roster import

Builds an NFL Head Coach 09 roster save from a modern player ratings export (the CSV from EA's
Madden ratings site), replacing every player on the 32 teams. The inputs (the CSV, the HC09
roster used as a template) and intermediate files land in this folder and `data/`, which git
ignores; the finished roster, disc patches and images in `hc27/` are committed, so players get
them with the install.

## Use

The result is played with **HC27.exe** (built with HC09.exe), which starts with this roster as
the game's default: everything it needs goes in the `hc27` folder of the install.

1. In HC09, save the default roster (**My Coach > File Management > Save**). The save is
   `build/userdata/content/00000001/ROSTER-<name>/ROSTER-<name>`; copy that file here.
2. Build the new roster:

   ```sh
   python tools/roster_import/import_roster.py --csv "tools/roster_import/Madden 27 Ratings.csv"        --roster tools/roster_import/ROSTER-DEFAULT --out tools/roster_import/data/ROSTER-M27        --report tools/roster_import/data/players.csv --contracts tools/roster_import/data/contracts.csv
   ```

   `--contracts` (optional) gives the players their real contracts. Make the CSV from Over The
   Cap's pages: save https://overthecap.com/contracts as `data/otc/contracts.html` and each team's
   https://overthecap.com/salary-cap/<team> page (e.g. `chicago-bears.html`), then

   ```sh
   python tools/roster_import/otc_contracts.py --pages tools/roster_import/data/otc        --out tools/roster_import/data/contracts.csv
   ```

   Each player gets his contract's length, and for each season left its cap number split into
   salary and prorated signing bonus, as the team pages show them (the game keeps up to seven
   seasons). The current season is the first one the pages show. A player is found by name on his
   team, or by name alone when he has moved, or by last name on his team when the first names
   differ but fit (Cam, Cameron). Players without a contract get the league minimum for their
   experience. The game does not count dead money (the cap charges of players a team has
   released), so a team that has a lot of it has that much more room than in reality. Without
   `--contracts`, the 2008 contracts are dealt out again (see below).

   The team ratings Team Select shows (overall, offense, defense, and the position groups) are
   worked out from each team's best players at each position, and spread over the same range
   as the 2008 ratings (the best team gets 2008's best value, the worst 2008's worst).

   The 2008 draft class the roster lists (by player id) is cleared, since those ids now belong to
   the imported players. Otherwise a career started in the preseason would take some of them off
   their teams as unsigned draft picks waiting for a contract.

3. Player portraits:

   ```sh
   python tools/roster_import/fetch_portraits.py --report tools/roster_import/data/players.csv        --roster tools/roster_import/data/ROSTER-M27
   ```

   downloads each player's image (the CSV's `avatarUrl`, cached in `data/portraits/`) and installs
   it as `hc27/textures/swappables/PlayerPortraits/<portrait id>.png`, which HC27 draws in place of
   that portrait. Run it again whenever the roster is rebuilt: portrait ids can change, and the
   folder is rebuilt to match. Players without an image (the site has none for a few dozen) are
   switched to the game's silhouette in the roster.

   The same step gives every player a 3D head of his skin tone (it needs Pillow, `pip install
   pillow`). Otherwise he would keep the head of the 2008 player whose record he took over. The
   game's generic heads come in light, medium and dark tones. The tone is measured on the face in
   the player's image, and the head is one the 2008 roster uses for that tone. The game has no
   screen that shows player heads without a helmet, so heads are matched by skin tone only, not
   by face. Skin colour alone misjudges some players (a light-skinned black player can measure
   like a tanned white one). To set those by hand, list them in a CSV and pass it with `--looks`:

   ```
   first,last,skin,head
   Caleb,Williams,medium,
   ```

   `skin` is `light`, `medium` or `dark`; `head` (optional) is a generic head number (`PGHE`).
4. Coaches: put a photo of each coach in `data/coaches/`, named `<team>_<role>_<first>_<last>.png`
   (e.g. `bears_hc_Ben_Johnson.png`; roles `hc`, `oc`, `dc`, `sc`), list the 3D head each should
   use in `data/coaches.csv` (columns `team,role,head,hat`, head `preset N` for create-a-coach
   appearance N or `likeness N` for a 2008 head coach's head 0-36, and hat `yes` or `no` for the
   cap he wears on New Career's Team Select, empty to keep the 2008 coach's), then write HC27's roster:

   ```sh
   python tools/roster_import/import_coaches.py --roster tools/roster_import/data/ROSTER-M27        --out hc27/ROSTER --heads tools/roster_import/data/coaches.csv --iso "iso/NFL Head Coach 09.iso"
   ```

   Each coach takes over the record of his team's coach in that role (name and head). His
   ratings and career record stay the 2008 coach's unless `--ratings
   tools/roster_import/data/coach_ratings.csv` gives them (columns and meanings in the script's
   help): overall, offense, defense, knowledge, motivation, charisma, work ethic, approval, base
   defense, career and playoff record, Super Bowls, years with the team, age, the position
   groups he develops best, and his contract (salary a year and seasons left; without them he
   keeps the 2008 coach's). Preset heads are read from the disc image. A coach without a `coaches.csv` row
   gets a preset of his skin tone (light or dark, measured on the face in his photo, as for
   players), so rows are only needed to pick a particular head or to correct the tone: for
   example `likeness 20` for Andy Reid, who has his own 2008 head. The photos become the coaches'
   portraits on the career screens such as Staff Status (head and shoulders, in
   `hc27/textures/swappables/CoachPortraits/`), and each head coach's photo also becomes his
   cut-out picture on Coach Now's Team Select (home and away, in
   `hc27/textures/swappables/TeamSelect/`): the plain background is removed and the coach framed
   like the game's own pictures. That needs Pillow (`pip install pillow`); a photo that already
   has a transparent background is used as it is. On New Career's Team Select, where the game draws each head coach in 3D with a body
   made for his head, a coach with a preset head wears the generic polo the game gives presets
   when creating a coach. Without coaches,
   copy `data/ROSTER-M27` to `hc27/ROSTER` instead.
5. Team logos (optional; the Commanders, Bears, Rams, Dolphins, Chargers, Jaguars, Falcons and Browns): the game's images of a team's logo and name
   are textures inside the disc's archives, so they are replaced by their contents. Run HC27 once
   with `set HC09_DUMP_HASHED=1` and go through the screens that show the team: Coach Now's Team
   Select and a game with the team (loading screen, Coach Agenda, play calling, score bug), and
   New Career's Team Select and a career with the team (its campus). Every texture drawn is saved
   in `build/hashed`. Then

   ```sh
   python tools/roster_import/team_logos.py --team commanders --logo tools/roster_import/data/logos/commanders.png \
       --end-zone tools/roster_import/data/logos/washington_endzone.png
   ```

   (and `--team bears --logo tools/roster_import/data/logos/chicago.png` for the Bears, whose
   helmets and midfield keep the C, and `--team rams --logo
   tools/roster_import/data/logos/losangelesrams.png` for the Rams, whose helmets keep the horns
   and whose end zones keep the name) puts the logo (a PNG with a transparent background) and the
   name in place of the old ones in
   the team's textures it knows (`TEAMS` in the script): menus, helmets, the coaches' polo shirts,
   the stadium's midfield logo and end zones (`--end-zone`, a PNG with a transparent background;
   without it the name is lettered there) and the career campus. It writes them to
   `hc27/textures/hashed`.
   It names any it did not find in the dump. The team's name itself comes from the roster
   (`TEAM_RENAMES` in `import_roster.py`).

   The Dolphins, Chargers, Jaguars, Falcons and Browns need no dump: their originals (menus' logos,
   the score bug's helmets, midfield, the players' helmet decal) are read from the disc by
   `team_uniforms.py --export` (step 6):

   ```sh
   python tools/roster_import/team_uniforms.py --export build/logo_originals --team dolphins chargers jaguars falcons browns
   python tools/roster_import/team_logos.py --team dolphins --logo tools/roster_import/data/logos/dolphins.png --dump build/logo_originals
   ```

   (and the same for `chargers`, `jaguars`, `falcons` and `browns`, with their logos).

   Every loading-screen backdrop of these teams (the game picks one of a team's at random; `BACKDROPS`
   in the script) is exported too; `--only backdrop` redoes just those, e.g. for the Bears, Rams and
   Commanders, whose other images are already done, and for the Seahawks and Jets, whose backdrops take
   their recoloured large logo (`--logo hc27/textures/hashed/38424377B24AE938.png`, `3C9DCE9A65C5C976.png`).
6. Uniforms (optional): the colours teams wear now, where they changed since 2008 (the Seahawks,
   Rams, Bills, Dolphins, Chargers, Jets, 49ers, Broncos, Jaguars and Browns; `RECOLOURS` and
   `SHELLS` in the script). Needs `pip install numpy xxhash pillow`.

   ```sh
   python tools/roster_import/team_uniforms.py
   ```

   reads each team's uniforms (every kit: jerseys, pants, socks, numbers, the sideline uniforms)
   and helmet decals from the disc, recolours them and writes them to `hc27/textures/hashed`; the
   helmets' shell and facemask colours are colours of their material, which it writes to
   `hc27/disc`. Some menu images (the Seahawks' and Jets' logos, the Rams' and Browns' score-bug
   helmets) are recoloured too. Run it before step 5's teams (it leaves their logos to
   `team_logos.py`). The menus' team colours come from the roster (`team_colours.py`, set by
   `import_roster.py`, or in an existing roster with `team_uniforms.py --roster hc27/ROSTER`).
   `--inspect <team>` lists the main colours of a team's kit textures, to write a new recipe.
7. Title screen (optional): with the title screen in `build/hashed` (HC27 run to Press Start with
   `HC09_DUMP_HASHED=1`),

   ```sh
   python tools/roster_import/title_screen.py --coach tools/roster_import/data/ui/title_ben_johnson.png \
       --title tools/roster_import/data/ui/title.png
   ```

   replaces the coach on it and the game's title (shown on a white banner), and removes the
   copyright line.
8. League finances: with the contracts at today's prices, the league's own amounts must be too:

   ```sh
   python tools/roster_import/league_salaries.py --iso "iso/NFL Head Coach 09.iso"
   ```

   sets the 2026 salary cap ($301.2M, against 2008's $116M), the minimum salaries by experience
   and the restricted free agent tenders, and raises the game's salary scales (what free agents,
   draft picks, re-signing players and hired coaches ask for) with the cap. They are tables in the
   disc's databases (listed in the script); it writes them, changed, to `hc27/disc`, which HC27
   lays over the disc's files at startup. The values are at the top of the script.
9. Last season and team descriptions (New Career's Team Select):

   ```sh
   python tools/roster_import/last_season.py --iso "iso/NFL Head Coach 09.iso"
   python tools/roster_import/team_descriptions.py --iso "iso/NFL Head Coach 09.iso"
   ```

   The first writes each team's 2025 record and how its season ended into the career history
   (`import_roster.py` also puts the record in the roster, which is what Team Select shows); the
   second replaces each team's description with a 2026 one. Both are at the top of the scripts
   (descriptions at most 255 characters, and all 32 must fit in the space the game's own took).
   They write their tables to `hc27/disc` too; each tool replaces only its own files there.
10. Run `build/hc09/HC27.exe`. Its saves are kept in `build/userdata-hc27`, apart from HC09's.

## Keeping up with the season

- `roster_moves.py` brings a roster up to date with the league's transactions (trades, waiver claims,
  signings, releases) from NFL.com's transaction pages, saved as a CSV: each player goes to the team
  of his latest transaction, and depth charts and team ratings are rebuilt. Players the roster does
  not have (practice squad and undrafted players missing from the ratings) cannot be added. The
  week 8 2026 roster was made with

  ```sh
  python tools/roster_import/roster_moves.py --roster hc27/ROSTER --transactions tools/roster_import/data/transactions_2026.csv \
      --since 2026/08/01 --out tools/roster_import/data/ROSTER-M27_week8 --report tools/roster_import/data/moves_week8.csv
  ```

  and is `hc27/ROSTER`, with its depth charts rebuilt and `draft_rounds.py` applied. The tools
  build depth charts no deeper than the game allows at each position (`POSITION_DEPTH` in
  `import_roster.py`, 91 entries a team); a deeper chart makes the game hang at the kickoff.
- `career_picks.py` gives new careers the real owners of the 2027 and 2028 draft picks (the trades
  are listed in the script). A career takes its picks from a template on the disc (the 2008 game's,
  with 2008's trades); `python tools/roster_import/career_picks.py --iso "iso/NFL Head Coach 09.iso"`
  writes the template to `hc27/disc` with the 2027 trades, and `hc27/draft_picks.txt` with the 2028
  ones, which HC27 gives a career the first time it is saved (a pre-season start makes the 2028
  picks itself; `src/common/draft_picks.cpp`). `--career <save>` instead applies both years' trades
  to an existing career saved in its first season (a `.bak` copy is kept).
- `rookie_scale.py` writes `hc27/rookie_scale.txt`, the rookie wage scale: for each of the draft's
  224 picks, a four-year deal's total value, signing bonus and yearly salaries, taken from the 2026
  draft class's real contracts in `hc27/ROSTER` (ranked by value, so the nth largest is the nth
  pick's). When a career's draft ends, HC27 signs every pick to his slot instead of leaving the
  teams to negotiate with their picks' agents (`src/common/rookie_scale.cpp`). Rerun it after
  rebuilding the roster with new contracts.
- `team_philosophies.py` sets each team's position philosophies (Workhorse HB, Pass Block Specialist
  Tackle, ...) to fit its depth chart. A team rates its players by its philosophy at each position,
  and the 2026 coaches kept the 2008 coaches' philosophies, so a backup could rate above the starter
  (the Bears' Workhorse HB put Kyle Monangai above D'Andre Swift). The script reads the game's
  player evaluation weights from the disc (tables PEIW and PEAW, one weight per rating for each of
  the 81 philosophies), scores each team's players under every philosophy for the position, and
  picks the one under which the starters rate highest against the rest of the position. Of those, it
  takes the one under which the players rate closest to their Madden overalls (`--players`, default
  `data/players.csv` and `data/free_agents.csv`), then the one that best keeps the depth chart's
  order, then the current one. The philosophies differ by a lot: Mobile/Speed QB rates backups 10 to
  15 points above their Madden overall, while Field General QB keeps them within a few points.
  The head coach (COCH) and the
  general manager (GMVW) both get it. A career takes its head coaches from the roster but its
  general managers from the career database on the disc, and in a career a team rates its players by
  the GM's philosophy, so the GMs' choices also go to `hc27/disc`. `--report` (default
  `data/philosophies.csv`) lists each choice, the starters' ranks and the distance from Madden. Run
  it after rebuilding depth charts (`roster_moves.py`):

  ```sh
  python tools/roster_import/team_philosophies.py --iso "iso/NFL Head Coach 09.iso"
  ```

  The weights only approximate the game's evaluation, which also counts height and the condition of
  each body part, among other things. A career saves the game's own scores (the overall its roster
  screens show) for every player under all 81 philosophies when it begins. For an exact choice, start
  a career from the roster (any team), save it, and pass the save with `--career`, e.g.
  `--career build/userdata-hc27/content/00000001/CAREER-QUICKSAVE/CAREER-QUICKSAVE`. A career keeps
  the philosophies it began with. Its roster screen's ratings also count each player's past production
  (below), so they need `player_stats.py` too.
- `player_stats.py` gives the players their real NFL statistics. A new career takes its players' past
  statistics (season lines and career totals: offense, defense, kicking and punting, returns, games)
  from the career database on the disc, by player id, and HC27's players took over the 2008 players'
  ids, so each started a career with the 2008 player's statistics; the game's player evaluation counts
  production, so the 49ers' Kurtis Rourke (a 2008 starter's 4,679 passing yards) rated 81 and Brock
  Purdy (a backup's 501) 73 on the roster screen. The script fills those tables with the players'
  regular-season statistics from [nflverse](https://github.com/nflverse/nflverse-data) (release
  `stats_player`, CC-BY 4.0, see THIRD_PARTY_NOTICES.md), matching players by name, position and team,
  and writes them to `hc27/disc`. The tables cannot grow, so their rows go to the players newest season
  first and, where a table is full (defensive career totals), to the players with the most recent playing
  time. Download the season files (`stats_player_reg_1999.csv` to `stats_player_reg_2025.csv`) into
  `data/nflverse`, then

  ```sh
  python tools/roster_import/player_stats.py --iso "iso/NFL Head Coach 09.iso"
  ```

  `--report` (default `data/player_stats.csv`) lists each team player and free agent and the nflverse player he was
  matched to (most of those without one are 2026 rookies).
- `free_agents.py` replaces the 2008 free agents (the roster's free-agent pool, which a career's free
  agency offers) with the league's real ones. EA's ratings site keeps a list for each week's ratings:
  the launch list has every player with Madden ratings, free agents included, and each week's list the
  players on a team that week. A player is a free agent when no team has him in the latest week's list
  nor in the roster, and the transactions (`roster_moves.py`'s CSV) do not put him on a team: released
  players and practice squad players (HC09 has none) are free agents, players waived injured or with a
  contract in `data/contracts.csv` but no transaction (on a reserve list) are not. Each takes his
  ratings from the latest list that has him. The roster has a fixed number of player records, so the
  best 512 (each position keeping its share of the pool) take over the 2008 free agents' records,
  unsigned. Portraits can only replace ids the game's portrait library has, and only those the 2008
  free agents had to themselves are left: they go first to imported players who had no image (the
  site has no Madden 27 image of some; `fetch_portraits.py` now takes their Madden 26 one), then to
  the best free agents; the rest show the silhouette, with a head of their skin tone. The week 8 pool
  was made with

  ```sh
  python tools/roster_import/free_agents.py --fetch
  python tools/roster_import/fetch_portraits.py --report tools/roster_import/data/players.csv --report tools/roster_import/data/free_agents.csv --roster hc27/ROSTER --looks tools/roster_import/data/looks.csv
  python tools/roster_import/player_stats.py --iso "iso/NFL Head Coach 09.iso"
  ```

  (`--fetch` downloads the lists into `data/ea_ratings`; without it the ones there are used). Run
  `fetch_portraits.py` with both reports from then on, or the free agents' portraits are removed.
- `draft_rounds.py` sets the draft round and pick (PDRO, PDPI) of the players still on their rookie
  contracts, which the imported players otherwise take from the 2008 records: ranking each class's
  four-year rookie deals by their value as signed (from `data/contracts.csv`) gives back the draft
  order. HC27's fifth-year option (`src/common/fifth_year_option.cpp`) needs the first round.
  `import_roster.py` runs it; `python tools/roster_import/draft_rounds.py` applies it to
  `hc27/ROSTER`, and `--save <career>` to a career started before it.

The roster can also be played in HC09 as a save: copy it to
`build/userdata/content/00000001/ROSTER-M27/ROSTER-M27`, create `ROSTER-M27.name` beside that
folder holding a name in UTF-16, and load it with **My Coach > File Management > Load** (HC09
does not show the portraits, which are HC27's).

## What is imported

- Names, team, position, jersey number, height, weight, age, birthday, years pro, handedness.
- Ratings, mapped to HC09's set (`RATINGS` in `import_roster.py`): most map one to one; throw
  accuracy is a weighted blend of short, medium and deep, elusiveness averages juke, spin and
  change of direction, and route running averages its three depths. Each rating's progression
  ceiling grows with how far a player is from 27 (faster for mental ratings).
- Depth charts are rebuilt from position and overall rating, including the return, kickoff,
  long-snap and third-down slots.
- Contracts, with `--contracts` (step 2): each player's real contract. Without it (the ratings
  CSV has none), each team's 2008 contracts are dealt out again by rank (best player, biggest cap
  hit), which keeps every team under the 2008 cap.
- Every imported player with an image gets a portrait id (`PSXP`) of his own, so his portrait
  can be replaced; the rest get the generic silhouette.

Not imported: colleges (HC09 stores an id into a list we have not mapped yet, so a player shows
the college of the record he took over), most of the appearance (equipment and build come from
that record; skin tone and head are set from the player's image, see step 3), and long snappers
(HC09 has no such position). Team names stay 2008's, except the Redskins, renamed the
Commanders; logos and uniforms are updated by steps 5 and 6 for the teams that changed theirs.

## File format

`tdb.py` reads and writes the saves. A save is an `MC02` container: a header with the file and
section sizes and three checksums, then a small section with a timestamp, then an EA TDB table
database (16 tables: `PLAY` players, `TEAM`, `DCHT` depth charts, contracts and ratings as
bit-packed fields). The container checksums are the game's CRC (`sub_82F0D220`); each table also
ends with a CRC-32 of its field definitions and records. The game silently ignores a roster whose
table CRC is wrong, so every edit goes through `SaveFile.fix_checksums()`.

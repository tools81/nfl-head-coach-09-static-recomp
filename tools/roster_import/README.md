# Roster import

Builds an NFL Head Coach 09 roster save from a modern player ratings export (the CSV from EA's
Madden ratings site), replacing every player on the 32 teams. No game or ratings data is included:
the CSV, the HC09 roster used as a template and everything generated land in this folder and
`data/`, which git ignores.

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
5. Team logos (optional; so far the Commanders, Bears and Rams): the game's images of a team's logo and name
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
6. Title screen (optional): with the title screen in `build/hashed` (HC27 run to Press Start with
   `HC09_DUMP_HASHED=1`),

   ```sh
   python tools/roster_import/title_screen.py --coach tools/roster_import/data/ui/title_ben_johnson.png \
       --title tools/roster_import/data/ui/title.png
   ```

   replaces the coach on it and the game's title (shown on a white banner), and removes the
   copyright line.
7. League finances: with the contracts at today's prices, the league's own amounts must be too:

   ```sh
   python tools/roster_import/league_salaries.py --iso "iso/NFL Head Coach 09.iso"
   ```

   sets the 2026 salary cap ($301.2M, against 2008's $116M), the minimum salaries by experience
   and the restricted free agent tenders, and raises the game's salary scales (what free agents,
   draft picks, re-signing players and hired coaches ask for) with the cap. They are tables in the
   disc's databases (listed in the script); it writes them, changed, to `hc27/disc`, which HC27
   lays over the disc's files at startup. The values are at the top of the script.
8. Last season and team descriptions (New Career's Team Select):

   ```sh
   python tools/roster_import/last_season.py --iso "iso/NFL Head Coach 09.iso"
   python tools/roster_import/team_descriptions.py --iso "iso/NFL Head Coach 09.iso"
   ```

   The first writes each team's 2025 record and how its season ended into the career history
   (`import_roster.py` also puts the record in the roster, which is what Team Select shows); the
   second replaces each team's description with a 2026 one. Both are at the top of the scripts
   (descriptions at most 255 characters, and all 32 must fit in the space the game's own took).
   They write their tables to `hc27/disc` too; each tool replaces only its own files there.
9. Run `build/hc09/HC27.exe`. Its saves are kept in `build/userdata-hc27`, apart from HC09's.

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
(HC09 has no such position). Team names, logos and uniforms stay 2008's, except the Redskins,
renamed the Commanders (with their logo, step 5).

## File format

`tdb.py` reads and writes the saves. A save is an `MC02` container: a header with the file and
section sizes and three checksums, then a small section with a timestamp, then an EA TDB table
database (16 tables: `PLAY` players, `TEAM`, `DCHT` depth charts, contracts and ratings as
bit-packed fields). The container checksums are the game's CRC (`sub_82F0D220`); each table also
ends with a CRC-32 of its field definitions and records. The game silently ignores a roster whose
table CRC is wrong, so every edit goes through `SaveFile.fix_checksums()`.

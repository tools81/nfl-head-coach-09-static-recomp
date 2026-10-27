"""Replaces the team descriptions on New Career's Team Select with HC27's.

    python tools/roster_import/team_descriptions.py --iso "iso/NFL Head Coach 09.iso"

They are the CCRI table of the game's databases (TDSC the description, TGID the team), a
compressed string field: the record holds an offset into the table's string area, which starts
with a Huffman tree (two bytes per branch, the node it leads to or 0 and a character; node 0 the
root) followed by the strings, each a byte of length and its codes, most significant bit first,
padded to a byte. The game's tree only has the characters of its own descriptions (no "-" or
"1"), so a new tree is built for the new ones; they must fit the string area. The table goes to
hc27/disc (see disc_patches.py).
"""

import argparse
import struct

import disc_patches
from last_season import TEAM_IDS

DESCRIPTIONS = {
    "Bears": "Ben Johnson turned the Bears around in his first year, winning the NFC North at 11-6 before an overtime "
             "loss to the Rams. Caleb Williams leads a young offense. Shore up the defensive "
             "line next.",
    "Bengals": "Joe Burrow and Ja'Marr Chase give the Bengals one of the most dangerous passing games in the league, but "
               "a 6-11 season showed how far the defense has fallen. Fix the defense and contend again.",
    "Bills": "Josh Allen carried the Bills to 12-5 and a Wild Card win before an overtime loss in Denver. Joe Brady takes "
             "over with an MVP quarterback and a solid roster. Add playmakers and finish the job.",
    "Broncos": "Sean Payton's Broncos went 14-3 and earned the AFC's top seed behind an elite defense "
               "before falling 10-7 to the Patriots in the AFC title game. The offense must improve.",
    "Browns": "The Browns struggled to a 5-12 finish and turn to Todd Monken to rebuild the offense. The defense has real "
              "talent, but the quarterback situation is unsettled. Build from the ground up.",
    "Buccaneers": "The Buccaneers faded to 8-9 and lost the NFC South on tiebreakers. Baker Mayfield and a strong offensive "
                  "line give Todd Bowles a good base. Upgrade the defensive line.",
    "Cardinals": "A 3-14 season cost the Cardinals their coach, and Mike LaFleur takes over a team in need of answers at "
                 "quarterback. There are good pieces at receiver and in the secondary.",
    "Chargers": "Jim Harbaugh has the Chargers winning again, going 11-6 before a Wild Card loss at New England. Justin "
                "Herbert leads a talented roster. Upgrade the running game to go deeper.",
    "Chiefs": "After years at the top, the Chiefs slipped to 6-11. Andy Reid and Patrick Mahomes "
              "are still here and the offense is dangerous. Rebuild the defense and contend right away.",
    "Colts": "The Colts finished 8-9 and just missed the playoffs. Shane Steichen has one of the best running games in "
             "the league and a deep offensive line. Settle the quarterback position.",
    "Cowboys": "The Cowboys finished 7-9-1 and missed the playoffs despite a high powered offense led by Dak Prescott "
               "and a deep group of receivers. Brian Schottenheimer needs a defense that can hold a lead.",
    "Dolphins": "The Dolphins went 7-10 and are starting over with Jeff Hafley as head coach. The roster is thin and the "
                "quarterback position is a question mark. A tough rebuild.",
    "Eagles": "The Eagles won the NFC East at 11-6 but were upset by the 49ers in the Wild Card round. Nick Sirianni has "
              "an elite offensive line and a deep roster. Get the passing game back on track.",
    "Falcons": "The Falcons went 8-9 and missed the playoffs, and Kevin Stefanski arrives to build a winner. A great "
               "running game is a good start. Find answers at quarterback and on defense.",
    "49ers": "The 49ers went 12-5 and won a Wild Card game in Philadelphia before being routed in Seattle. Kyle Shanahan "
             "has elite weapons. Strengthen the secondary to get past the Seahawks.",
    "Giants": "The Giants finished 4-13 and hired John Harbaugh to turn things around. Young quarterback Jaxson Dart and "
              "a talented defense give the new staff something to build on.",
    "Jaguars": "Liam Coen led a remarkable turnaround as the Jaguars went 13-4 and won the AFC South before a narrow Wild "
               "Card loss to Buffalo. Build up the lines to go deeper.",
    "Jets": "A 3-14 season left the Jets near the bottom of the league. Aaron Glenn has good linebackers to work with, "
            "but the offense needs help everywhere, starting at quarterback.",
    "Lions": "The Lions slipped to 9-8 and missed the playoffs after years of success. Dan Campbell still has a "
             "talented roster and a powerful running game. Fix the defense and Detroit contends.",
    "Packers": "The Packers went 9-7-1 and lost a Wild Card shootout in Chicago. Matt LaFleur has Jordan Love and a "
               "balanced roster. Rebuild the offensive line and Green Bay is right back in the race.",
    "Panthers": "The Panthers won the NFC South at 8-9 and pushed the Rams to the limit in a 34-31 Wild Card loss. Dave "
                "Canales has a young team on the rise led by Bryce Young. Keep adding talent around him.",
    "Patriots": "Mike Vrabel brought the Patriots all the way back, going 14-3 and winning the AFC before losing Super "
                "Bowl LX to Seattle. Drake Maye leads a team with a great secondary. A great job.",
    "Raiders": "The Raiders went 3-14 and turn to Klint Kubiak to fix the offense. There is talent at receiver and on the "
               "offensive line, but the roster needs work at quarterback and on defense. A tough job.",
    "Rams": "The Rams went 12-5 and came within one game of the Super Bowl, falling 31-27 in Seattle. Sean McVay has "
            "Matthew Stafford and a dominant defensive front. Win now.",
    "Ravens": "The Ravens missed the playoffs at 8-9 and start a new era under Jesse Minter. Lamar Jackson and an elite "
              "linebacker group give this team a high ceiling. Fix the offensive line.",
    "Redskins": "The Commanders fell to 5-12 a year after reaching the NFC title game. Dan Quinn has Jayden Daniels and "
                "good receivers, but the defense needs an overhaul. Rebuild the front seven first.",
    "Saints": "The Saints went 6-11 in a rebuilding year under Kellen Moore, and young quarterback Tyler Shough showed "
              "promise. There are holes across the roster, so this will take some time.",
    "Seahawks": "The Seahawks are Super Bowl champions after a 14-3 season and a 29-13 win over the Patriots in Super "
                "Bowl LX. Mike Macdonald has a deep defense and great receivers. Now defend the title.",
    "Steelers": "The Steelers won the AFC North at 10-7 but were blown out by Houston in the Wild Card round. Mike "
                "McCarthy takes over a team built on a dominant defense. Find a long term answer at quarterback.",
    "Titans": "The Titans went 3-14 and hand the rebuild to Robert Saleh. Young quarterback Cam Ward is the future, and "
              "the defense has some talent. Build the offensive line around him and be patient.",
    "Vikings": "The Vikings finished 9-8 and just missed the playoffs. Kevin O'Connell has some of the best receivers in "
               "the game and a solid defense. The quarterback position will decide how far this team goes.",
    "Texans": "The Texans went 12-5 and crushed the Steelers in the Wild Card round before losing at New England. DeMeco "
              "Ryans has one of the best defenses in football. The offense needs to catch up.",
}


def build_tree(texts):
    """A Huffman tree for the texts' characters, in the game's layout: node n's branches at
    entries 2n and 2n + 1, node 0 the root."""
    import heapq
    from collections import Counter
    counts = Counter("".join(texts))
    heap = [(count, i, char) for i, (char, count) in enumerate(sorted(counts.items()))]
    heapq.heapify(heap)
    serial = len(heap)
    while len(heap) > 1:
        a, b = heapq.heappop(heap), heapq.heappop(heap)
        heapq.heappush(heap, (a[0] + b[0], serial, (a[2], b[2])))
        serial += 1
    nodes = [heap[0][2]]  # breadth first, so the root is node 0
    entries = []
    for node in nodes:
        for child in node:
            if isinstance(child, tuple):
                nodes.append(child)
                entries.append(bytes([len(nodes) - 1, 0]))
            else:
                entries.append(bytes([0, ord(child)]))
    if len(nodes) > 255:
        raise SystemExit("too many different characters for the tree")
    return b"".join(entries)


def huffman_codes(tree):
    """Character -> bit string, from the tree's branches."""
    codes = {}

    def walk(node, prefix):
        for bit in (0, 1):
            entry = tree[(node * 2 + bit) * 2:(node * 2 + bit) * 2 + 2]
            if entry[0] == 0:
                codes[chr(entry[1])] = prefix + str(bit)
            else:
                walk(entry[0], prefix + str(bit))
    walk(0, "")
    return codes


def encode(text, codes):
    missing = sorted({c for c in text if c not in codes})
    if missing:
        raise SystemExit(f"characters the game's text has no code for: {missing!r} in {text[:40]!r}...")
    if len(text) > 255:
        raise SystemExit(f"{len(text)} characters, more than 255: {text[:40]!r}...")
    bits = "".join(codes[c] for c in text)
    bits += "0" * (-len(bits) % 8)
    return bytes([len(text)]) + bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


def update(db):
    t = db.tables.get("CCRI")
    if not t or not t.count:
        return []
    end = dict((table.name, end) for table, end in db.table_extents())["CCRI"] - 4  # before the table's CRC
    area = t.data_end
    texts = {TEAM_IDS[name]: text for name, text in DESCRIPTIONS.items()}
    tree = build_tree(texts.values())
    codes = huffman_codes(tree)
    first = len(tree)
    strings, offset = bytearray(), first
    for i in range(t.count):
        tgid = t.get(i, "TGID")
        if tgid not in texts:
            raise SystemExit(f"no description for team {tgid}")
        coded = encode(texts[tgid], codes)
        struct.pack_into(">I", db.data, t.data_start + i * t.record_bytes, offset)
        strings += coded
        offset += len(coded)
    if area + first + len(strings) > end:
        raise SystemExit(f"the descriptions take {first + len(strings)} bytes, more than the {end - area} the table has")
    db.data[area:end] = tree + bytes(strings) + bytes(end - area - first - len(strings))
    return ["CCRI"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iso", required=True, help="the game's disc image")
    parser.add_argument("--out", default=disc_patches.OUT)
    args = parser.parse_args()
    found = disc_patches.databases(args.iso, ["CCRI"])
    if not found:
        raise SystemExit(f"no team descriptions found in {args.iso}")
    written = disc_patches.write_patches("team_descriptions", [(path, offset, db, update(db)) for path, offset, db in found],
                                         args.out)
    print(f"{len(DESCRIPTIONS)} team descriptions written to {written} tables in {args.out}")


if __name__ == "__main__":
    main()

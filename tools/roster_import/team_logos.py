"""Replaces a team's logo in HC27's images with a new one.

    python tools/roster_import/team_logos.py --team commanders \
        --logo tools/roster_import/data/logos/commanders.png --dump build/hashed

The game's images of a team's logo are not files of their own: they are textures inside the
disc's archives, so HC27 replaces them by their contents. Run HC27 once with HC09_DUMP_HASHED=1
through the screens that show the team (see README.md): every texture it draws is saved as
build/hashed/<content hash>.png. This script takes the team's textures from there (TEAMS lists
them by hash), puts the new logo (a PNG with a transparent background) in place of the old one,
and writes the results to hc27/textures/hashed/<content hash>.png, which HC27 draws instead.
The originals stay on the user's machine. Needs Pillow (pip install pillow).
"""

import argparse
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# Content hash -> how the logo (and name) sits in that texture:
#   logo      the logo alone on a transparent background (the 3D-style team logos)
#   flat      the same, a flat logo
#   panel     the logo on a flat colour
#   helmet    a helmet with the logo on its side
#   backdrop  a stadium picture with the logo laid over its top left, see-through
#   wordmark  the team's name lettered at the top of a decal sheet
#   sign      a line of text, the team's name and the logo on cloth
#   banner    the team's name down a band on the left, the logo on the right
#   end zone  a field with the team's name in the end zone at its top
#   field     the stadium's field: midfield logo, and the end zones (--end-zone, else the name)
#   midfield  the stadium's field: the midfield logo only
#   decal     the logo alone, mirrored (the helmet's decal)
#   boxes     logos at the given boxes, see replace_boxes (written ("boxes", box, ...))
#   ribbon    a ribbon board with the logo repeated along it
#   shell     a logo on a plain surface such as a helmet's shell, in the given box
TEAMS = {
    "commanders": {
        "043CD6AD867A5001": "logo",      # large logo: New Career's Team Select
        "181C216DFB917D7C": "logo",      # small logo: Coach Now's Team Select, lists
        "FBF16677D07C8432": "flat",      # the logo icon; the coaches' polo shirts
        "58247B363D5442E8": "panel",     # logo on burgundy: Coach Agenda, play calling, career screens
        "5BEF0B7DCCE6B946": "helmet",    # the score bug's helmet
        "EA62130CC9A1831D": "backdrop",  # loading screen
        "4925A48C122E18A8": "wordmark",  # uniform decals
        "C066B100A9F13131": "sign",      # career campus: "Home of the ..."
        "EDE68C0076B3EBB3": "banner",    # career campus banners
        "9C9864C294ADDFD6": "end zone",  # career campus practice field
        "D5AF025EC8685384": "field",     # the stadium's field
        "20589A6B61EAEF09": "decal",     # the players' helmets
        "7A644E59B25586AF": ("shell", (45, 74, 112, 166)),  # the score bug's helmet
    },
    # The Bears keep their C on helmets and at midfield (not listed), and on the stadium's facade,
    # where the C is the sign's shape (its texture only colours it).
    "bears": {
        "030E84D7313F2B3A": "logo",      # large logo: Team Select, faint behind Coin Toss and play calling
        "B18615265D8E92B7": "logo",      # small logo: Team Select, lists, Coach Agenda
        "5B2729EFE128B24A": "flat",      # the logo icon
        "CC75441A35AB985E": "backdrop",  # loading screen
        "E916E72B7C9B62D3": "ribbon",    # the stadium's ribbon boards
        "ECAB9F952B19071D": "ribbon",
        "CDD19846AEEB5863": ("boxes", (50, 268, 258, 474), (50, 511, 256, 650)),  # stadium banners
        "12399CE37820B4D6": ("boxes", (44, 122, 200, 230)),                       # career campus: "Home of the ..."
        "AEBF6C88B77B2C49": ("boxes", (6, 148, 252, 304, "down")),               # career campus banner
        "E0BB93021F429A64": ("boxes", (3, 79, 35, 113)),                          # career campus banner
    },
    # The Rams keep their horns on helmets (the score bug's 06040080E0A6CB4F and the players'
    # CDF9FA900A84C62D, not listed) and the ram-shaped sign on the career campus building
    # (1CF24E3C4CE6B901, whose shape is the model's). The "St. Louis Rams" lettering beside a logo
    # goes with it (one new logo for both); "Rams" alone (end zones, the campus wall) stays.
    "rams": {
        "47477AB0E71E123F": "logo",      # large logo: Team Select
        "B297981FB4B83143": "logo",      # small logo: Team Select, lists, Coach Agenda, clipboard
        "4F1695F5F8954377": "flat",      # the logo icon
        "5BBC5F3314361FEE": "midfield",  # the stadium's field
        "5104F93738182D03": ("boxes", (100, 24, 766, 490)),                       # logo on navy, the horn beside it
        "FD234BB4F19EAE73": ("boxes", (46, 6, 213, 123), (902, 6, 1069, 123),     # the stadium's ribbon boards
                             (2982, 6, 3149, 123), (3838, 6, 4005, 123)),
        "5CAD4F617ADA01F3": ("boxes", (392, 26, 503, 71)),                        # stadium: championship banners
        "9BBEB0CF8B9AA86B": ("boxes", (100, 478, 208, 592)),                      # coaches' apparel
        "A52F21BB760B4658": ("boxes", (100, 478, 208, 592)),
        "27D7950D18DA10E8": ("boxes", (50, 42, 210, 222)),                        # career campus: "Home of the ..."
        "55A242AB4C86CC67": ("boxes", (3, 27, 61, 102)),                          # career campus banners
        "75EFA5EEFE3B9D04": ("boxes", (136, 373, 194, 414)),                      # career campus building wall
    },
    # Teams whose small logo (Team Select, lists, Coach Agenda) is replaced so far; the other
    # places their logo appears are still to be found in a dump.
    "cardinals": {
        "B5351947433DF688": "logo",      # small logo: Team Select, lists, Coach Agenda
        "951DF05C94A80ED6": "logo",  # 3D logo
        "EBE172BCCF33FAE9": "decal",  # logo, stored mirrored
        "1EA627001701C713": ("shell", (146, 91, 219, 146)),  # the score bug's helmet
        "47517631E337CC2C": "field",  # the stadium's field
    },
    "falcons": {
        "BB973DC788C0476C": "logo",      # small logo: Team Select, lists, Coach Agenda
        "E64E9F724634FBC1": "decal",  # logo, stored mirrored
        "9D90738A0BA084C1": ("shell", (131, 73, 203, 151)),  # the score bug's helmet
        "2C2B5F6B2257581A": "field",  # the stadium's field
    },
    "ravens": {
        "94AA8E58348087DA": "logo",      # small logo: Team Select, lists, Coach Agenda
        "28BA7323CDA017D1": "decal",  # logo, stored mirrored
        "71D4730D9D8F63B3": "decal",  # logo, stored mirrored
        "76A67056A9659E09": "logo",  # 3D logo
        "D4CBA95B39FD971F": ("shell", (42, 90, 128, 142)),  # the score bug's helmet
    },
    "bills": {
        "D870BA40EF7AB5DB": "logo",      # small logo: Team Select, lists, Coach Agenda
        "577435681A544CDB": "logo",  # 3D logo
        "5F5C02CB4DF6199F": "decal",  # logo, stored mirrored
        "802CF3A9E3BB0B07": "logo",  # 3D logo
        "745F36AF9DCDDE8F": ("shell", (128, 69, 215, 157)),  # the score bug's helmet
        "B478E6798C8F6D1E": "field",  # the stadium's field
    },
    "panthers": {
        "0DD6777226F82FAA": "logo",      # small logo: Team Select, lists, Coach Agenda
        "796D129F2B866DA7": "logo",  # 3D logo
        "C5A1117E928DD5F1": "decal",  # logo, stored mirrored
        "A4167A7633905F7D": ("shell", (47, 84, 113, 152)),  # the score bug's helmet
    },
    "bengals": {
        "3D87516609E9209A": "logo",      # small logo: Team Select, lists, Coach Agenda
        "A5E2B5FA242D74EE": "logo",  # 3D logo
        "E6FCE618171869C2": "field",  # the stadium's field
    },
    "browns": {
        "4D0C2360B3F896FA": "logo",      # small logo: Team Select, lists, Coach Agenda
        "ED3F12BE876BFB7B": "field",  # the stadium's field
    },
    "cowboys": {
        "72C0C8A00E48547F": "logo",      # small logo: Team Select, lists, Coach Agenda
        "ABA05666755E32D7": "logo",  # 3D logo
        "8C45A9F38B504266": ("shell", (140, 74, 212, 150)),  # the score bug's helmet
        "E9D8A8B8CC25B508": "field",  # the stadium's field
    },
    "broncos": {
        "A2C57BF042160006": "logo",      # small logo: Team Select, lists, Coach Agenda
        "68729C89082D0B5E": "logo",  # 3D logo
        "C48414EE7EBFA3BF": ("shell", (133, 90, 215, 142)),  # the score bug's helmet
        "85A84DD229E4E5F7": "field",  # the stadium's field
    },
    "lions": {
        "3E2FF804292AC0E5": "logo",      # small logo: Team Select, lists, Coach Agenda
        "91B5F5D16285F06A": "decal",  # logo, stored mirrored
        "38E79523358F020C": ("shell", (130, 75, 215, 159)),  # the score bug's helmet
        "0DA0EF2ADBB958EB": "field",  # the stadium's field
    },
    "packers": {
        "3AE5FAF75E3A07D5": "logo",      # small logo: Team Select, lists, Coach Agenda
        "DF7C2A952335BE2B": "logo",  # 3D logo
        "A786F69AAB261FA8": ("shell", (47, 84, 119, 138)),  # the score bug's helmet
    },
    "texans": {
        "EDC7E00CF6209B8E": "logo",      # small logo: Team Select, lists, Coach Agenda
        "419A3B447743FC42": "decal",  # logo, stored mirrored
        "EAAE5C6EDBE41928": "logo",  # 3D logo
        "91C57669DE04C7F4": ("shell", (43, 73, 119, 146)),  # the score bug's helmet
    },
    "colts": {
        "801F1B48D4F29B36": "logo",      # small logo: Team Select, lists, Coach Agenda
        "C592EF28A08A0AB0": "flat",      # the logo icon
        "B6F9B1993F823F1B": ("shell", (148, 66, 208, 136)),                      # the score bug's helmet
        "8AE16B7CBED42753": ("boxes", (66, 120, 172, 230)),                      # career campus: "Home of the ..."
        "9C9A658BA133E30A": ("boxes", (88, 132, 190, 236)),                       # logo on blue under the name
    },
    "jaguars": {
        "D818446A0440AA2A": "logo",      # small logo: Team Select, lists, Coach Agenda
        "327C66BA521F177A": "logo",  # 3D logo
        "3909F9C1F016B6B8": ("shell", (58, 63, 132, 109)),  # the score bug's helmet
    },
    "chiefs": {
        "790DA51D0A004450": "logo",      # small logo: Team Select, lists, Coach Agenda
        "CFF772F02E010845": "logo",  # 3D logo
        "D651AC828D5E96BF": "decal",  # logo, stored mirrored
        "E427ADAC5CC41685": "decal",  # logo, stored mirrored
        "879CCF9E3AEF803A": ("shell", (131, 84, 211, 146)),  # the score bug's helmet
    },
    "dolphins": {
        "FC1881A7129AA603": "logo",      # small logo: Team Select, lists, Coach Agenda
        "EF371B4C6D420EFC": "logo",  # 3D logo
        "7D6D7A6129C3F961": ("shell", (143, 80, 202, 161)),  # the score bug's helmet
        "E41A97891C456E34": "field",  # the stadium's field
    },
    "vikings": {
        "7A916E0854398C66": "logo",      # small logo: Team Select, lists, Coach Agenda
        "476091EDFFD0A92F": "logo",  # 3D logo
    },
    "patriots": {
        "323F2D2D5B528374": "logo",      # small logo: Team Select, lists, Coach Agenda
        "7EB6A78AA3A25B26": "logo",      # large logo
        "2F696785D161310E": ("shell", (34, 70, 112, 138)),                       # the score bug's helmet
        "3324AD566FA87588": "decal",     # the players' helmets
    },
    "saints": {
        "2061F69B5A560CB2": "logo",      # small logo: Team Select, lists, Coach Agenda
        "C84A969989BCBD3A": "logo",  # 3D logo
        "E30A9EB753167251": "logo",  # 3D logo
        "0930DB437FCED6B4": ("shell", (49, 74, 112, 143)),  # the score bug's helmet
    },
    "giants": {
        "0ABD07BFCCDF215F": "logo",      # small logo: Team Select, lists, Coach Agenda
        "AA072F58BD0D6E34": ("shell", (144, 86, 203, 140)),  # the score bug's helmet
        "D8394B0B84BE4344": "field",  # the stadium's field
    },
    "raiders": {
        "762A85F720265012": "logo",      # small logo: Team Select, lists, Coach Agenda
        "7F8F9870EA463A1E": "logo",  # 3D logo
        "D760423941445565": "logo",  # 3D logo
        "D45D20F95C48CD2B": ("shell", (51, 74, 105, 137)),  # the score bug's helmet
    },
    "eagles": {
        "99B9865814C0069E": "logo",      # small logo: Team Select, lists, Coach Agenda
        "88FF8E9075C7A7E9": "field",  # the stadium's field
    },
    "steelers": {
        "B218C4B9EE8AD7B9": "logo",      # small logo: Team Select, lists, Coach Agenda
        "21EB6CC23222C27F": "logo",  # 3D logo
        "985D446C9348107D": "logo",  # 3D logo
        "96EECD092C7AF8C2": ("shell", (50, 76, 108, 134)),  # the score bug's helmet
    },
    "chargers": {
        "5695BF9684F8886E": "logo",      # small logo: Team Select, lists, Coach Agenda
        "9EF3F3026E965245": "logo",  # 3D logo
        "FAA71CEB1C88D56B": "logo",  # 3D logo
        "B73A500772A1D6AE": "field",  # the stadium's field
    },
    "49ers": {
        "DAEE42605EE09C35": "logo",      # small logo: Team Select, lists, Coach Agenda
        "AD549DA6C5781975": "logo",  # 3D logo
        "4E7B9CBF1593CE76": ("shell", (48, 84, 120, 131)),  # the score bug's helmet
    },
    "seahawks": {
        "11BF384F47C617DB": "logo",      # small logo: Team Select, lists, Coach Agenda
        "2F795C9C6C113931": "decal",  # logo, stored mirrored
        "CDC0DD91DA45B9B3": ("shell", (36, 64, 126, 156)),  # the score bug's helmet
    },
    "buccaneers": {
        "7D3CCAEF042B0A77": "logo",      # small logo: Team Select, lists, Coach Agenda
        "318D040448CEC4FF": "decal",  # logo, stored mirrored
        "8537CCC0EF560506": "logo",  # 3D logo
        "AE1957420D762BB2": ("shell", (129, 68, 211, 147)),  # the score bug's helmet
        "563AFB31967E8762": "field",  # the stadium's field
    },
    "titans": {
        "0D0D085CBBBE58AE": "logo",      # small logo: Team Select, lists, Coach Agenda
        "44294828A43C002A": "logo",  # 3D logo
        "F58ABF97AC49F746": "logo",  # 3D logo
        "90BB1A348371AF75": ("shell", (43, 90, 118, 153)),  # the score bug's helmet
    },
    "jets": {
        "4849D2A237EDAA3F": "logo",      # small logo: Team Select, lists, Coach Agenda
        "5531BE6712E2FE0D": "logo",  # 3D logo
        "AE58A1AD760E3EE1": ("shell", (55, 87, 126, 136)),  # the score bug's helmet
    },
}


def fitted(logo, width, height):
    """The logo scaled to fit width x height, keeping its shape."""
    from PIL import Image
    scale = min(width / logo.width, height / logo.height)
    return logo.resize((max(1, round(logo.width * scale)), max(1, round(logo.height * scale))), Image.LANCZOS)


def paste_centred(canvas, logo, box, opacity=1.0):
    left, top, right, bottom = box
    logo = fitted(logo, right - left, bottom - top)
    if opacity < 1:
        logo.putalpha(logo.getchannel("A").point(lambda v: round(v * opacity)))
    canvas.alpha_composite(logo, ((left + right - logo.width) // 2, (top + bottom - logo.height) // 2))


def opaque_box(image, threshold=16):
    return image.getchannel("A").point(lambda v: 255 if v > threshold else 0).getbbox()


def replace_logo(original, logo):
    """A 3D-style logo: the new logo where the old one was, with a soft shadow under it."""
    from PIL import Image, ImageFilter
    left, top, right, bottom = opaque_box(original)
    picture = Image.new("RGBA", original.size, (0, 0, 0, 0))
    sized = fitted(logo, right - left, bottom - top)
    x, y = (left + right - sized.width) // 2, (top + bottom - sized.height) // 2
    offset = max(1, original.width // 100)
    shadow = Image.new("RGBA", original.size, (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 255), (x + offset, y + offset), sized.getchannel("A"))
    shadow = shadow.filter(ImageFilter.GaussianBlur(offset * 2))
    shadow.putalpha(shadow.getchannel("A").point(lambda v: v // 2))
    picture.alpha_composite(shadow)
    picture.alpha_composite(sized, (x, y))
    return picture


def replace_panel(original, logo):
    """The old logo painted over with the panel's colour, the new one in its place."""
    from PIL import ImageChops, ImageDraw
    background = original.getpixel((2, 2))
    flat = original.copy()
    flat.paste(background, (0, 0) + original.size)
    differs = ImageChops.difference(original.convert("RGB"), flat.convert("RGB")).convert("L").point(lambda v: 255 if v > 40 else 0)
    left, top, right, bottom = differs.getbbox()
    picture = original.copy()
    ImageDraw.Draw(picture).rectangle((left - 4, top - 4, right + 4, bottom + 4), fill=background)
    paste_centred(picture, logo, (left, top, right, bottom))
    return picture


def replace_helmet(original, logo):
    """The decal on the helmet's side (the logo's ring, right of the facemask) covered in the
    helmet's colour, the new logo on it."""
    from PIL import ImageDraw
    width, height = original.size
    pixels = original.load()
    ring = [(x, y) for y in range(height) for x in range(width // 2, width)
            if pixels[x, y][3] > 128 and pixels[x, y][0] > 190 and 130 < pixels[x, y][1] < 215 and pixels[x, y][2] < 90]
    xs, ys = [p[0] for p in ring], [p[1] for p in ring]
    left, top, right, bottom = min(xs), min(ys), max(xs) + 1, max(ys) + 1
    # The helmet's colour: the reddest opaque pixels around the decal.
    around = [pixels[x, y] for y in range(max(0, top - 8), min(height, bottom + 8)) for x in range(max(0, left - 8), min(width, right + 8))
              if not (left <= x < right and top <= y < bottom) and pixels[x, y][3] > 200 and pixels[x, y][0] > 1.8 * pixels[x, y][1]]
    around.sort(key=sum)
    shell = around[len(around) // 2] if around else (110, 20, 40, 255)
    from PIL import Image, ImageChops
    patch = Image.new("L", original.size, 0)
    ImageDraw.Draw(patch).ellipse((left - 1, top - 1, right + 1, bottom + 1), fill=255)
    # Only on the shell: what lies outside the helmet, and its dark outline where the decal's
    # edge meets it, stay.
    shell_only = original.getchannel("A").point(lambda v: 255 if v > 200 else 0)
    outline = original.convert("L").point(lambda v: 0 if v < 40 else 255)
    inner = Image.new("L", original.size, 0)
    ImageDraw.Draw(inner).ellipse((left + 3, top + 3, right - 3, bottom - 3), fill=255)
    patch = ImageChops.multiply(ImageChops.multiply(patch, shell_only), ImageChops.lighter(outline, inner))
    picture = original.copy()
    picture.paste(shell, (0, 0) + original.size, patch)
    margin = (right - left) // 12
    paste_centred(picture, logo, (left + margin, top + margin, right - margin, bottom - margin))
    return picture


def replace_backdrop(original, logo):
    """The see-through logo over the picture's top left covered with the picture's right side
    (mirrored, blended in at the edges), then the new logo laid over it the same way."""
    from PIL import Image, ImageOps
    width, height = original.size
    cover_width, cover_height = width // 2, round(height * 0.62)
    cover = ImageOps.mirror(original.crop((width - cover_width, 0, width, cover_height)))
    fade = max(8, width // 12)
    mask = Image.new("L", (cover_width, cover_height), 255)
    for x in range(cover_width - fade, cover_width):
        for y in range(cover_height):
            mask.putpixel((x, y), round(255 * (cover_width - x) / fade))
    for y in range(cover_height - fade // 2, cover_height):
        for x in range(cover_width):
            mask.putpixel((x, y), min(mask.getpixel((x, y)), round(255 * (cover_height - y) / (fade // 2))))
    picture = original.copy()
    picture.paste(cover, (0, 0), mask)
    paste_centred(picture, logo, (width * 8 // 100, height * 7 // 100, width * 47 // 100, height * 58 // 100), opacity=0.4)
    return picture


def replace_flat_logo(original, logo):
    """A flat logo alone on a transparent background: the new one where it was."""
    from PIL import Image
    picture = Image.new("RGBA", original.size, (0, 0, 0, 0))
    paste_centred(picture, logo, opaque_box(original))
    return picture


def replace_decal(original, logo):
    """The logo alone, stored mirrored (as the helmet's decal is): the new one, mirrored."""
    from PIL import ImageOps
    return replace_flat_logo(original, ImageOps.mirror(logo))


def text_image(text, fill, outline=None):
    """The text in a bold font, large, on transparency, cropped to the letters."""
    from PIL import Image, ImageDraw, ImageFont
    font = None
    for name in ("impact.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf"):
        try:
            font = ImageFont.truetype(name, 200)
            break
        except OSError:
            continue
    font = font or ImageFont.load_default()
    stroke = 12 if outline else 0
    canvas = Image.new("RGBA", (200 * len(text) + 100, 320), (0, 0, 0, 0))
    ImageDraw.Draw(canvas).text((50, 40), text, font=font, fill=fill, stroke_width=stroke, stroke_fill=outline)
    return canvas.crop(opaque_box(canvas))


def median_colour(pixels):
    pixels = sorted(pixels, key=lambda p: p[0] + p[1] + p[2])
    return pixels[len(pixels) // 2] if pixels else (128, 128, 128, 255)


def matching_box(image, box, test):
    """Bounding box (within box) of the pixels that pass test."""
    pixels = image.load()
    xs, ys = [], []
    for y in range(box[1], box[3]):
        for x in range(box[0], box[2]):
            if test(pixels[x, y]):
                xs.append(x)
                ys.append(y)
    return (min(xs), min(ys), max(xs) + 1, max(ys) + 1) if xs else None


def paint_over(image, box, vertical=True):
    """The box filled from the pixels just outside it (between the rows above and below, or the
    columns left and right of it), keeping the cloth's shading."""
    left, top, right, bottom = box
    pixels = image.load()
    width, height = image.size
    for y in range(top, bottom):
        for x in range(left, right):
            if vertical:
                a, b = pixels[x, max(0, top - 2)], pixels[x, min(height - 1, bottom + 1)]
                t = (y - top + 1) / (bottom - top + 1)
            else:
                a, b = pixels[max(0, left - 2), y], pixels[min(width - 1, right + 1), y]
                t = (x - left + 1) / (right - left + 1)
            pixels[x, y] = tuple(round(a[c] * (1 - t) + b[c] * t) for c in range(4))


def stretched(image, box):
    from PIL import Image
    return image.resize((box[2] - box[0], box[3] - box[1]), Image.LANCZOS)


def gold(p):
    return p[3] > 128 and p[0] > 190 and 120 < p[1] < 225 and p[2] < 110


def replace_wordmark(original, logo, name):
    """The team's name lettered at the top of a decal sheet (the rest, the maker's name, stays)."""
    width, height = original.size
    box = opaque_box(original.crop((0, 0, width, height // 2)))
    pixels = original.load()
    region = [pixels[x, y] for y in range(box[1], box[3]) for x in range(box[0], box[2])]
    fill = median_colour([p for p in region if p[3] > 200 and p[0] > 1.5 * p[1] and p[0] < 190])
    outline = median_colour([p for p in region if gold(p)])
    picture = original.copy()
    picture.paste((0, 0, 0, 0), box)
    picture.alpha_composite(stretched(text_image(name.upper(), fill, outline), box), box[:2])
    return picture


def replace_sign(original, logo, name):
    """A sign: a line of text, the team's name in its colour below it, and the logo below that."""
    width, height = original.size
    name_box = matching_box(original, (0, height // 8, width, height * 2 // 5), gold)
    pixels = original.load()
    colour = median_colour([pixels[x, y] for y in range(name_box[1], name_box[3]) for x in range(name_box[0], name_box[2])
                            if gold(pixels[x, y])])
    background = median_colour([pixels[x, y] for y in range(height * 2 // 5, height) for x in (0, 1, width - 2, width - 1)])
    logo_box = matching_box(original, (0, name_box[3] + 4, width, height),
                            lambda p: sum(abs(p[c] - background[c]) for c in range(3)) > 90)
    picture = original.copy()
    paint_over(picture, (name_box[0] - 2, name_box[1] - 2, name_box[2] + 2, name_box[3] + 2), vertical=False)
    paint_over(picture, (logo_box[0] - 2, logo_box[1] - 2, logo_box[2] + 2, logo_box[3] + 2), vertical=False)
    lettering = text_image(name.upper(), colour)
    picture.alpha_composite(stretched(lettering, name_box), name_box[:2])
    paste_centred(picture, logo, logo_box)
    return picture


def replace_banner(original, logo, name):
    """A banner: the team's name down a coloured band on the left, the logo on the right above
    its stripes."""
    width, height = original.size
    band = width * 45 // 100
    name_box = matching_box(original, (0, 0, band, height), lambda p: p[0] > 1.6 * p[1] and p[0] < 210)
    pixels = original.load()
    colour = median_colour([pixels[x, y] for y in range(name_box[1], name_box[3]) for x in range(name_box[0], name_box[2])
                            if pixels[x, y][0] > 1.6 * pixels[x, y][1] and pixels[x, y][0] < 210])
    # The right side down to its stripes (the first row below the middle that is mostly not cloth)
    # holds the logo: covered in the cloth's colour, the new logo on it.
    def cloth(p):
        return p[0] > 1.6 * p[1] and p[0] < 220
    stripes = next((y for y in range(height // 2, height)
                    if sum(not cloth(pixels[x, y]) for x in range(band, width)) > (width - band) * 3 // 4), height)
    right = (band + 2, 0, width, stripes - 1)
    flat = median_colour([pixels[x, y] for y in range(right[1], right[3]) for x in range(right[0], right[2]) if cloth(pixels[x, y])])
    picture = original.copy()
    paint_over(picture, (name_box[0] - 1, name_box[1], name_box[2] + 1, name_box[3]), vertical=False)
    picture.paste(flat, right)
    lettering = text_image(name.upper(), colour).rotate(-90, expand=True)
    picture.alpha_composite(stretched(lettering, name_box), name_box[:2])
    margin = (width - band) // 10
    paste_centred(picture, logo, (right[0] + margin, right[1] + margin, right[2] - margin, right[3] - margin))
    return picture


def replace_end_zone(original, logo, name):
    """A field with the team's name in the end zone at its top: the grass beside the name painted
    over it, the name lettered again in its colour."""
    width, height = original.size

    def lettering_colour(p):  # the letters' red, shaded parts included
        return p[0] > 60 and p[0] > 1.8 * p[1]
    pixels = original.load()
    # The rows of the name: from the first with lettering in it to the next without.
    rows = [sum(lettering_colour(pixels[x, y]) for x in range(width)) > width // 50 for y in range(height // 3)]
    top = rows.index(True)
    bottom = rows.index(False, top) if False in rows[top:] else len(rows)
    name_box = matching_box(original, (0, top, width, bottom), lettering_colour)
    colour = median_colour([pixels[x, y] for y in range(name_box[1], name_box[3]) for x in range(name_box[0], name_box[2])
                            if lettering_colour(pixels[x, y]) and pixels[x, y][0] > 110])
    picture = original.copy()
    margin = max(2, width // 100)
    paint_over(picture, (name_box[0] - margin, name_box[1] - margin, name_box[2] + margin, name_box[3] + margin), vertical=False)
    picture.alpha_composite(stretched(text_image(name.upper(), colour), name_box), name_box[:2])
    return picture


def field_white(p):
    return min(p[:3]) > 150


def centre_shape(image, box, test):
    """Bounding box of the shape at the box's centre: the pixels passing test that are joined
    (allowing small gaps) to those nearest the centre."""
    from collections import deque
    left, top, right, bottom = box
    pixels = image.load()
    w, h = right - left, bottom - top
    hit = [[test(pixels[left + x, top + y]) for x in range(w)] for y in range(h)]
    cx, cy = w // 2, h // 2
    seeds = sorted(((x, y) for y in range(h) for x in range(w) if hit[y][x]),
                   key=lambda q: (q[0] - cx) ** 2 + (q[1] - cy) ** 2)[:1]
    if not seeds:
        return None
    gap = max(2, w // 30)
    seen = {seeds[0]}
    queue = deque(seeds)
    while queue:
        x, y = queue.popleft()
        for dy in range(-gap, gap + 1):
            for dx in range(-gap, gap + 1):
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < h and (nx, ny) not in seen and hit[ny][nx]:
                    seen.add((nx, ny))
                    queue.append((nx, ny))
    xs = [q[0] for q in seen]
    ys = [q[1] for q in seen]
    return left + min(xs), top + min(ys), left + max(xs) + 1, top + max(ys) + 1


def replace_midfield(original, logo):
    """The stadium's field, end zones at the top and bottom: the midfield logo covered with the
    field a few yard lines away (same lines and hash marks) and the new logo put on it, turned as
    the old one was. The end zones stay."""
    from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageStat
    width, height = original.size
    pixels = original.load()
    picture = original.copy()
    centre = width // 2
    # Midfield: yard lines down a column beside the logo; the logo is what is redder than the
    # grass (its burgundy, gold and brown) around the centre.
    line_column = centre - width * 8 // 100
    lines = [start for start, _ in runs_of([field_white(pixels[line_column, y]) for y in range(height)])]
    gaps = sorted(b - a for a, b in zip(lines, lines[1:]) if b - a > height // 100)
    spacing = gaps[len(gaps) // 2]  # the usual gap (lines near the end zones can be closer)
    reach = width // 10
    def logo_pixel(p):  # neither grass nor a white line: any colour of the old logo
        grass = p[1] > p[0] + 5 and p[1] >= p[2]
        return not grass and not field_white(p) and (max(p[:3]) - min(p[:3]) > 40 or max(p[:3]) < 70)
    logo_box = centre_shape(original, (centre - reach, height // 2 - reach, centre + reach, height // 2 + reach),
                            logo_pixel)
    margin = width // 100
    box = (logo_box[0] - margin, logo_box[1] - margin, logo_box[2] + margin, logo_box[3] + margin)
    # From whole yard-line gaps away, clear of the logo, up or down the field: the patch whose
    # edges match the field around the box best, blended in at its edges.
    first = -(-(box[3] - box[1]) // spacing)
    ring = Image.new("L", original.size, 0)
    ImageDraw.Draw(ring).rectangle(box, outline=255, width=3)
    ring = ring.crop(box)

    def mismatch(shift):
        patch = original.crop((box[0], box[1] + shift, box[2], box[3] + shift))
        around = original.crop(box)  # the box's own edge is still field (the logo is inside)
        return sum(ImageStat.Stat(ImageChops.difference(patch, around).convert("L"), ring).mean)

    # Measured from line to line (the gaps vary by a pixel or two).
    through = min(range(len(lines)), key=lambda i: abs(lines[i] - (box[1] + box[3]) // 2))
    shifts = [lines[through + k] - lines[through] for k in range(-first - 2, first + 3)
              if abs(k) >= first and 0 <= through + k < len(lines)]
    # Patches from the field's middle only (the end zones' lettering is further out).
    shifts = [shift for shift in shifts if height // 5 <= box[1] + shift and box[3] + shift <= height - height // 5]
    shift = min(shifts, key=mismatch)
    feather = Image.new("L", (box[2] - box[0], box[3] - box[1]), 0)
    edge = margin // 2
    ImageDraw.Draw(feather).rectangle((edge, edge, feather.width - 1 - edge, feather.height - 1 - edge), fill=255)
    feather = feather.filter(ImageFilter.GaussianBlur(edge / 2))
    picture.paste(original.crop((box[0], box[1] + shift, box[2], box[3] + shift)), box[:2], feather)
    span_x, span_y = logo_box[2] - logo_box[0], logo_box[3] - logo_box[1]
    # As big as the old logo by area (a long one such as a leaping tiger would make a square logo
    # huge), and never more than a fourteenth of the field's width.
    size = min(max(span_x, span_y) * 13 // 10, int((span_x * span_y) ** 0.5 * 1.4), width // 14)
    turned = logo.rotate(90, expand=True)  # the logo's top toward the field's left, as the old one
    paste_centred(picture, turned, ((logo_box[0] + logo_box[2] - size) // 2, (logo_box[1] + logo_box[3] - size) // 2,
                                    (logo_box[0] + logo_box[2] + size) // 2, (logo_box[1] + logo_box[3] + size) // 2))
    return picture


def team_colour(logo):
    """The logo's main colour: its most common strongly coloured (or dark) pixel."""
    from collections import Counter
    small = logo.convert("RGBA").resize((64, 64))
    counts = Counter((p[0] // 24, p[1] // 24, p[2] // 24) for p in small.getdata()
                     if p[3] > 200 and (max(p[:3]) - min(p[:3]) > 60 or max(p[:3]) < 60))
    if not counts:
        return (40, 40, 40, 255)
    r, g, b = counts.most_common(1)[0][0]
    return (r * 24 + 12, g * 24 + 12, b * 24 + 12, 255)


def replace_field(original, logo, name, end_zone=None):
    """The stadium's field: the midfield logo replaced (replace_midfield), and each end zone's art
    covered with its own grass and the end zone picture put in it (the bottom one turned around)."""
    from PIL import Image
    width, height = original.size
    pixels = original.load()
    white, runs = field_white, runs_of

    def grass(p):
        return p[1] > p[0] + 5 and p[1] > p[2] and not white(p)

    picture = replace_midfield(original, logo)
    centre = width // 2

    # End zones: inside the white border at each end, down the centre column.
    column = runs([white(pixels[centre, y]) for y in range(height)])
    zones = [(column[0][1], column[1][0], 0), (column[-2][1], column[-1][0], 180)]
    for top, bottom, turn in zones:
      try:
          row = [white(pixels[x, top + 2]) for x in range(width)]  # above the art
          left = max(x for x in range(centre) if row[x]) + 1
          right = min(x for x in range(centre, width) if row[x])
          clean = [x for x in range(left, right) if all(grass(pixels[x, y]) for y in range(top + 2, bottom - 2))]
          strips = runs([x in set(clean) for x in range(width)])
          start, end = max(strips, key=lambda r: r[1] - r[0])
          patch = original.crop((start, top, end, bottom))
          x, flip = left, False
          while x < right:
              piece = patch.transpose(Image.FLIP_LEFT_RIGHT) if flip else patch
              picture.paste(piece.crop((0, 0, min(piece.width, right - x), piece.height)), (x, top))
              x += piece.width
              flip = not flip
          art = end_zone if end_zone is not None else text_image(name.upper(), team_colour(logo), (255, 255, 255, 255))
          art = art.rotate(turn, expand=True)
          inset = (bottom - top) // 10
          paste_centred(picture, art, (left + inset, top + inset, right - inset, bottom - inset))
      except ValueError:  # an end zone with no plain grass to cover its art with: kept
          continue
    return picture


def replace_boxes(original, logo, *boxes):
    """Logos at known places: each box (left, top, right, bottom[, "down"]) is covered with the
    colours just left and right of it (or above and below it, "down": for a background that
    changes across, such as a diagonal) and the new logo is put in it."""
    picture = original.copy()
    for box in boxes:
        vertical = len(box) > 4 and box[4] == "down"
        paint_over(picture, box[:4], vertical=vertical)
        paste_centred(picture, logo, box[:4])
    return picture


def replace_ribbon(original, logo):
    """A ribbon board with the logo repeated along it (the Bears' C, as tall as the board): each
    covered with the board's background and the new logo put in its place. A logo is found by
    its colour down the columns: a wide band of columns filled most of the board's height (its
    stroke), and the bands of shorter columns just after it (its arms); text and stripes have no
    such band."""
    width, height = original.size
    pixels = original.load()

    def colour(p):  # the logo's orange
        return p[0] > 200 and 60 < p[1] < 140 and p[2] < 70
    counts = [sum(colour(pixels[x, y]) for y in range(height)) for x in range(width)]
    bands = [(start, end, max(counts[start:end])) for start, end in runs_of([c > 4 for c in counts])]
    background = median_colour([pixels[x, y] for x in range(0, width, 7) for y in range(0, height, 7)
                                if not colour(pixels[x, y]) and min(pixels[x, y][:3]) < 200])
    picture = original.copy()
    wide = height // 6
    k = 0
    while k < len(bands):
        start, end, tallest = bands[k]
        k += 1
        if end - start < wide or tallest < height * 6 // 10:
            continue
        while k < len(bands) and bands[k][0] - end < height // 2 and bands[k][1] - bands[k][0] >= wide                 and bands[k][2] < height // 2:
            end = bands[k][1]
            k += 1
        margin = height // 30  # the white outline
        box = (max(0, start - height // 8), 0, min(width, end + margin), height)  # the C's point reaches left
        picture.paste(background, box)
        paste_centred(picture, logo, (box[0], margin, box[2], height - margin))
    return picture


def runs_of(values):
    """(start, end) of each run of True."""
    out, start = [], None
    for i, v in enumerate(list(values) + [False]):
        if v and start is None:
            start = i
        elif not v and start is not None:
            out.append((start, i))
            start = None
    return out


def replace_shell(original, logo, box):
    """A logo on a plain surface (a helmet's shell): inside box, every pixel unlike the surface's
    colour (the median along the box's edge) becomes that colour, and the new logo goes in the box."""
    left, top, right, bottom = box
    picture = original.copy()
    px = picture.load()
    edge = [px[x, y] for x in range(left, right) for y in (top, bottom - 1)] + \
           [px[x, y] for y in range(top, bottom) for x in (left, right - 1)]
    edge = [p for p in edge if p[3] > 200]
    shell = median_colour(edge)
    for y in range(top, bottom):
        for x in range(left, right):
            p = px[x, y]
            if p[3] > 200 and sum(abs(p[i] - shell[i]) for i in range(3)) > 45:
                px[x, y] = tuple(shell[:3]) + (p[3],)
    paste_centred(picture, logo, box)
    return picture


RECIPES = {"shell": replace_shell, "logo": replace_logo, "flat": replace_flat_logo, "panel": replace_panel, "helmet": replace_helmet,
           "backdrop": replace_backdrop, "wordmark": replace_wordmark, "sign": replace_sign, "banner": replace_banner,
           "end zone": replace_end_zone, "field": replace_field, "midfield": replace_midfield, "decal": replace_decal,
           "boxes": replace_boxes, "ribbon": replace_ribbon}
LETTERED = {"wordmark", "sign", "banner", "end zone", "field"}  # recipes that also letter the team's name


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--team", required=True, choices=sorted(TEAMS))
    parser.add_argument("--logo", required=True, help="the new logo: a PNG with a transparent background")
    parser.add_argument("--end-zone", help="the end zones' picture (a PNG with a transparent background); "
                                           "without it the team's name is lettered there")
    parser.add_argument("--dump", default=os.path.join(ROOT, "build", "hashed"), help="HC09_DUMP_HASHED's folder")
    parser.add_argument("--out", default=os.path.join(ROOT, "hc27", "textures", "hashed"))
    args = parser.parse_args()
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("team_logos.py needs Pillow: pip install pillow")

    logo = Image.open(args.logo).convert("RGBA")
    box = opaque_box(logo)
    if not box:
        raise SystemExit(f"{args.logo} is empty")
    logo = logo.crop(box)
    end_zone = None
    if args.end_zone:
        end_zone = Image.open(args.end_zone).convert("RGBA")
        end_zone = end_zone.crop(opaque_box(end_zone))
    os.makedirs(args.out, exist_ok=True)
    missing = []
    for content_hash, kind in TEAMS[args.team].items():
        original_path = os.path.join(args.dump, f"{content_hash}.png")
        if not os.path.exists(original_path):
            missing.append(content_hash)
            continue
        original = Image.open(original_path).convert("RGBA")
        kind, *extra = kind if isinstance(kind, tuple) else (kind,)
        if kind in LETTERED:
            extra = [args.team] + extra
        if kind == "field" and end_zone:
            extra.append(end_zone)
        RECIPES[kind](original, logo, *extra).save(os.path.join(args.out, f"{content_hash}.png"))
        print(f"{content_hash} ({kind}) replaced")
    if missing:
        print(f"not in {args.dump} (run HC27 with HC09_DUMP_HASHED=1 through the screens that show them): "
              + ", ".join(missing))


if __name__ == "__main__":
    main()

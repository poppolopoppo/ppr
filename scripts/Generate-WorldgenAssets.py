"""Rebuild the original PPR v2 content pack (Python 3.11+, standard library only).

All patterns are periodic in both axes. Normal maps derive from the exact height
field used by albedo; PNG row zero corresponds to world Y zero. Run from any cwd.
The authored recipes here are the source of truth for the generated TOML/PNGs.
"""
from __future__ import annotations

import hashlib
import argparse
import math
from pathlib import Path
import struct
import tomllib
import zlib

ROOT = Path(__file__).resolve().parents[1] / "assets/worldgen/default"

# Explicit permanent ledger: the original seventeen keys retain their IDs.
# key, ID, tint, density, pattern family, roughness, roundness
SOLIDS = [
    ("basalt",16,"58636FFF",2900,"rock",.87,.65),
    ("sandstone",32,"B99A6CFF",2200,"sediment",.92,.90),
    ("copper",49,"BD7751FF",8900,"metal",.30,.60),
    ("iron",73,"8E7270FF",7800,"metal",.43,.55),
    ("ice",128,"A4D9EFA0",917,"ice",.18,.85),
    ("masonry",257,"AAA294FF",2400,"masonry",.85,.15),
    ("obsidian",509,"403D50FF",2600,"crystal",.14,.45),
    ("clay",65,"A86D4AFF",1800,"sediment",.95,1.0),
    ("limestone",97,"C9C5ADFF",2600,"sediment",.84,.75),
    ("salt",160,"F5E9D6DA",2160,"crystal",.36,.80),
    ("sulfur",193,"E3C74FFF",2070,"granular",.88,.80),
    ("crystal",385,"B69BE88A",2650,"crystal",.12,.25),
    ("granite",600,"9A9296FF",2700,"rock",.83,.45),
    ("shale",601,"6A7779FF",2550,"sediment",.91,.55),
    ("dirt",602,"8C6546FF",1450,"organic",1.0,.95),
    ("algae",603,"7EAD4AFF",1100,"organic",.89,.95),
    ("slime",604,"72974DE0",1350,"organic",.31,1.0),
    ("coal",605,"34333DFF",1300,"rock",.91,.65),
    ("rust",606,"C66F3FFF",2400,"granular",.94,.65),
    ("phosphorite",607,"B4B669FF",3100,"granular",.87,.65),
    ("aluminum",608,"C4CDD1FF",2700,"metal",.27,.40),
    ("wolframite",609,"5C657FFF",7300,"metal",.48,.40),
    ("diamond",610,"DAEEFF65",3510,"crystal",.04,.15),
    ("clear_quartz",611,"E4E9FA60",2650,"crystal",.10,.25),
    ("glass",612,"A2D8E04A",2500,"ice",.06,.30),
    ("polluted_ice",613,"91BCA7CE",940,"ice",.38,.85),
    ("iron_ore",614,"AD7F62FF",4500,"ore",.76,.55),
    ("copper_ore",615,"9C8755FF",3800,"ore",.74,.60),
]
# key, ID, tint, density, phase, family, roughness, IOR, effective depth
PARTICLES = [
    ("sand",1013,"D8BA78FF",1600,"solid","granular",1.0,1.5,0.0),
    ("snow",1019,"E7F3FFFF",300,"solid","granular",.97,1.31,0.0),
    ("pebbles",1031,"8F8B83FF",1700,"solid","rock",.96,1.5,0.0),
    ("regolith",1100,"B7A59AFF",1500,"solid","granular",1.0,1.5,0.0),
    ("ash",1101,"77737FFF",650,"solid","granular",1.0,1.5,0.0),
    ("water",1001,"3989C970",1000,"liquid","liquid",.12,1.333,.20),
    ("polluted_water",1200,"859E476C",1030,"liquid","liquid",.34,1.36,.20),
    ("brine",1201,"7DBAB57C",1200,"liquid","liquid",.16,1.38,.24),
    ("crude_oil",1202,"4C3D4DC4",850,"liquid","liquid",.26,1.47,.12),
    ("magma",1203,"FF6728E8",2800,"liquid","liquid",.48,1.52,.10),
    ("oxygen",1009,"A7DAD01C",1.2,"gas","gas",1.0,1.0003,0.0),
    ("polluted_oxygen",1300,"B5C56535",1.3,"gas","gas",1.0,1.0004,0.0),
    ("carbon_dioxide",1301,"9A96AF24",1.8,"gas","gas",1.0,1.00045,0.0),
    ("hydrogen",1302,"DDA4C328",.09,"gas","gas",1.0,1.00014,0.0),
    ("chlorine",1303,"D5E27938",3.2,"gas","gas",1.0,1.00077,0.0),
    ("steam",1304,"CFDCE54A",.60,"gas","gas",1.0,1.00026,.03),
    ("natural_gas",1305,"C7A36E2A",.80,"gas","gas",1.0,1.00044,0.0),
]


def table(lines, name, values):
    lines.append(f"\n[{name}]")
    for key, value in values.items():
        if isinstance(value, bool):
            value = str(value).lower()
        elif isinstance(value, str):
            value = '"' + value + '"'
        else:
            value = repr(value)
        lines.append(f"{key} = {value}")


def save(name, lines):
    (ROOT / name).write_text("\n".join(lines) + "\n", encoding="utf-8")


def materials():
    lines = ["schema_version = 2", "# Stable numeric-ID ledger is in Generate-WorldgenAssets.py."]
    optical = {"ice":(1.31,.18),"polluted_ice":(1.34,.15),"salt":(1.54,.06),
               "crystal":(1.54,.25),"diamond":(2.42,.28),"clear_quartz":(1.46,.24),
               "glass":(1.50,.30),"slime":(1.38,.08)}
    for key, ident, tint, density, family, rough, rounded in SOLIDS:
        ior, depth = optical.get(key,(1.5,0.0))
        display = {"copper":"Native copper","iron":"Native iron","aluminum":"Native aluminum",
                   "crystal":"Amethyst","salt":"Rock salt"}.get(key,key.replace("_"," ").capitalize())
        table(lines,"materials."+key,dict(numeric_id=ident,display_name=display,phase="solid",
              representation="terrain",color_rgba=tint,albedo_texture=f"textures/{key}.png",
              normal_texture=f"textures/{key}_normal.png",texture_period_m=1.0 if family=="crystal" else 2.0,
              roundness=rounded,diggable=key!="masonry",reference_density_kg_m3=float(density),
              metallic=1.0 if family=="metal" else .0,roughness=rough,emissive_r=.0,emissive_g=.0,
              emissive_b=.0,emissive_strength=1.0,alpha_mode="opaque" if tint[-2:]=="FF" else "blend",
              ior=ior,normal_scale=.65 if depth else 1.0,refraction_depth_m=depth))
    for key, ident, tint, density, phase, family, rough, ior, depth in PARTICLES:
        magma=key=="magma"
        table(lines,"materials."+key,dict(numeric_id=ident,display_name=key.replace("_"," ").capitalize(),
              phase=phase,representation="particle",color_rgba=tint,reference_density_kg_m3=float(density),
              albedo_texture=f"textures/{key}.png",normal_texture=f"textures/{key}_normal.png",
              texture_period_m=3.0 if phase=="gas" else 2.0,metallic=.0,roughness=rough,
              emissive_r=1.0 if magma else .0,emissive_g=.19 if magma else .0,emissive_b=.025 if magma else .0,
              emissive_strength=2.5 if magma else 1.0,alpha_mode="opaque" if phase=="solid" else "blend",
              ior=ior,normal_scale=.0 if phase=="gas" else .8,refraction_depth_m=depth))
    save("materials.toml",lines)


# biome, label, preview, T, delta, host, secondary, resources, lining, thickness,
# composition field, cave field, cave threshold, depth range, minimum start distance
BIOMES = [
    ("hearth","Hearth basin","9DB279FF",293,8,"sandstone","dirt",["clay","copper_ore","coal","algae"],"sandstone",1,"beds","round_caves",.56,(.0,.42),0),
    ("verdant","Verdant galleries","86B76AFF",300,8,"dirt","limestone",["algae","phosphorite"],"clay",1,"organic_beds","branch_caves",.05,(.04,.45),100),
    ("mire","Billowed mire","7E9754FF",305,10,"slime","clay",["shale","coal"],"clay",1,"organic_beds","low_pools",-.08,(.18,.65),160),
    ("dunes","Ochre dunes","C4A16BFF",315,15,"sandstone","shale",["rust","iron_ore"],"shale",1,"beds","tall_caves",.57,(.02,.46),200),
    ("rime","Rime shelves","8CBED9FF",248,15,"ice","granite",["polluted_ice","wolframite"],"granite",2,"frozen_beds","ice_fractures",.72,(.02,.48),240),
    ("lode","Dense lode country","B18B64FF",318,18,"granite","basalt",["iron_ore","copper_ore","copper","iron","aluminum","wolframite"],"basalt",3,"rock_masses","ore_tunnels",.78,(.36,.78),240),
    ("karst","Crystal karst","B8A4D2FF",286,12,"limestone","clear_quartz",["crystal","diamond"],"limestone",1,"karst_masses","karst_cells",.18,(.20,.74),150),
    ("saltworks","Evaporite shelves","D5C7A1FF",305,12,"salt","limestone",["clay"],"salt",2,"salt_beds","salt_basins",-.04,(.22,.72),200),
    ("fumaroles","Sulfur chimneys","C9AB43FF",365,25,"basalt","obsidian",["sulfur","rust"],"obsidian",2,"rock_masses","chimney_fissures",.59,(.56,.92),380),
    ("petroleum","Capped petroleum basins","7A665DFF",370,20,"shale","basalt",["coal","diamond"],"basalt",3,"oil_caps","oil_channels",.04,(.63,.94),400),
    ("ember","Ember depths","B6644EFF",1100,100,"basalt","obsidian",["glass","iron"],"obsidian",3,"rock_masses","hot_chambers",.66,(.82,1.0),500),
]


def fields():
    lines=["schema_version = 2", "# Coordinates are transformed before domain warp; global depth bands remain global."]
    specs={
        "composition":("open_simplex2","fbm",.055,1,1,0,2),
        "temperature":("perlin","fbm",.008,1,1,0,0),
        "caverns":("open_simplex2","ridged",.021,1,1,0,8),
        "fissures":("perlin","fbm",.038,1,1,0,3),
        "ore_lenses":("cellular_distance","none",.07,1,1,0,0),
        "warp_x":("open_simplex2","fbm",.006,1,1,0,0),
        "warp_y":("perlin","fbm",.006,1,1,0,0),
        "beds":("perlin","fbm",.014,.45,1.7,-7,14),
        "organic_beds":("open_simplex2","billow",.014,1.0,1.3,11,18),
        "frozen_beds":("perlin","fbm",.013,.6,1.7,4,12),
        "rock_masses":("open_simplex2","ridged",.018,1.2,.9,-23,9),
        "karst_masses":("open_simplex2","fbm",.012,1.1,.85,17,12),
        "salt_beds":("perlin","fbm",.011,.5,1.5,-3,14),
        "oil_caps":("perlin","fbm",.012,.55,1.35,-6,16),
        "round_caves":("open_simplex2","ridged",.012,1.0,1.0,0,18),
        "branch_caves":("open_simplex2","billow",.014,.8,1.2,27,18),
        "low_pools":("open_simplex2","billow",.010,.7,1.3,-4,20),
        "tall_caves":("open_simplex2","ridged",.016,1.8,.65,8,14),
        "ice_fractures":("perlin","ridged",.013,.7,1.4,7,17),
        "ore_tunnels":("perlin","ridged",.018,1.25,.8,-28,9),
        "karst_cells":("open_simplex2","fbm",.010,1.1,.85,17,28),
        "salt_basins":("open_simplex2","billow",.010,.7,1.1,-9,22),
        "chimney_fissures":("perlin","ridged",.018,1.7,.6,2,18),
        "oil_channels":("open_simplex2","billow",.012,.75,1.2,-14,20),
        "hot_chambers":("open_simplex2","ridged",.009,.8,1.05,-12,25),
        "ore_grain":("open_simplex2","fbm",.034,1.4,.7,35,7),
        "ore_seams":("perlin","ridged",.027,.5,1.5,-18,8),
        "organic_patches":("open_simplex2","billow",.030,1.2,.9,0,9),
    }
    for key,(algo,fractal,freq,sx,sy,rot,warp) in specs.items():
        table(lines,"fields."+key,dict(algorithm=algo,fractal=fractal,frequency_per_metre=freq,
              octaves=2 if key in {b[-4] for b in BIOMES} else 3,lacunarity=2.0,gain=.5,warp_amplitude_m=float(warp),
              warp_frequency_per_metre=.014 if key in {b[-4] for b in BIOMES} else .009,axis_scale_x=float(sx),axis_scale_y=float(sy),rotation_degrees=float(rot)))
    save("fields.toml",lines)


def biomes():
    lines=["schema_version = 2"]
    for key,label,color,temp,delta,host,secondary,resources,lining,width,field,*_ in BIOMES:
        prefix="biomes."+key
        table(lines,prefix,dict(display_name=label,preview_color_rgba=color,temperature_base_k=float(temp),
              temperature_variation_k=float(delta),temperature_field="temperature",lining_material=lining,
              lining_thickness_fine=width))
        for band,lo,hi,weights in [("upper",0.0,.50,(8.0,2.5)),("lower",.50,1.0,(6.0,3.5))]:
            p=prefix+".composition."+band
            table(lines,p,dict(order=10 if band=="upper" else 20,depth_min_normalized=lo,depth_max_normalized=hi,field=field))
            for material,weight in zip((host,secondary),weights):
                table(lines,p+".materials."+material,dict(material=material,weight=weight))
        for i,resource in enumerate(resources):
            f="organic_patches" if resource in ("algae","coal","slime") else "ore_seams" if key=="lode" else "ore_grain"
            # Nonoverlapping narrow windows produce localized resources instead of a weighted palette.
            lo=-.58+i*.165 if f!="ore_seams" else .59+i*.055
            hi=lo+(.095 if f!="ore_seams" else .035)
            table(lines,prefix+".deposits."+resource,dict(kind="threshold",order=10+i,field=f,material=resource,
                  host_material=host,depth_min_normalized=0.0,depth_max_normalized=1.0,
                  threshold_min=round(lo,4),threshold_max=round(hi,4)))
    save("biomes.toml",lines)


# Templates are authored independently of noise. Each contains one or more
# explicit structures and sealed matter anchors; no runtime fluid behavior.
FEATURES={}


def feature(key,label,w,h,ops,role="sealed_pocket",anchor=None):
    FEATURES[key]=dict(label=label,w=w,h=h,ops=ops,role=role,anchor=anchor or (w//2,h//2))


def ellipse(action,x,y,w,h,material=None):
    d=dict(action=action,shape="ellipse",x_fine=x,y_fine=y,width_fine=w,height_fine=h)
    if material: d["material"]=material
    return d


def rect(action,x,y,w,h,material=None):
    d=ellipse(action,x,y,w,h,material)
    d["shape"]="rectangle"
    return d


def author_features():
    # Keep protected landing geometry/markers and both resource volumes exactly.
    feature("landing","Hearth landing",90,36,[rect("paint",0,0,90,36,"masonry"),
            rect("carve",3,6,84,27),rect("paint",6,0,9,6,"sandstone"),rect("paint",75,0,9,6,"copper")],"start",(45,10))
    # key, host shell, dimensions, interior style, resource ledge.
    specs=[
        ("pocket","sandstone",66,45,"round","clay"),
        ("hearth_shelf","sandstone",84,42,"shelf","coal"),
        ("verdant_bulb","dirt",66,72,"branch","algae"),
        ("verdant_root","limestone",90,48,"branch","phosphorite"),
        ("mire_pool","clay",108,39,"basin","slime"),
        ("mire_chimney","shale",48,96,"chimney","coal"),
        ("dune_vault","shale",54,90,"chimney","rust"),
        ("dune_bed","sandstone",90,39,"shelf","iron_ore"),
        ("rime_shelf","granite",102,39,"shelf","polluted_ice"),
        ("ruin","ice",60,63,"round","wolframite"),
        ("room","granite",84,45,"shelf","copper_ore"),
        ("lode_lens","basalt",72,54,"round","aluminum"),
        ("crystal_geode","limestone",72,66,"geode","crystal"),
        ("quartz_geode","granite",60,54,"geode","clear_quartz"),
        ("salt_basin","salt",114,42,"basin","clay"),
        ("mine_gallery","limestone",96,39,"shelf","salt"),
        ("vent","obsidian",45,108,"chimney","sulfur"),
        ("fumarole_cluster","basalt",78,90,"branch","rust"),
        ("oil_reservoir","basalt",108,45,"basin","coal"),
        ("oil_channels","shale",96,54,"branch","diamond"),
        ("magma_vault","obsidian",102,63,"round","glass"),
        ("ember_fracture","basalt",60,90,"chimney","iron"),
    ]
    # Independent normalized chamber lobes, authored for each landmark.
    # Overlap joins lobes to the central anchor; no uniform cross-shaped recipe.
    lobes={
        "pocket":[(.06,.18,.64,.67),(.36,.09,.52,.76),(.20,.40,.53,.51)],
        "hearth_shelf":[(.07,.24,.61,.61),(.42,.15,.48,.56),(.55,.44,.34,.43)],
        "verdant_bulb":[(.15,.18,.65,.67),(.48,.07,.37,.50),(.07,.52,.51,.38)],
        "verdant_root":[(.10,.28,.67,.51),(.55,.13,.36,.61),(.14,.11,.40,.42)],
        "mire_pool":[(.07,.25,.64,.59),(.42,.30,.50,.58),(.27,.13,.41,.48)],
        "mire_chimney":[(.27,.08,.49,.61),(.13,.43,.55,.48),(.48,.23,.38,.48)],
        "dune_vault":[(.20,.13,.58,.68),(.10,.45,.50,.44),(.47,.07,.39,.43)],
        "dune_bed":[(.07,.20,.73,.59),(.51,.39,.40,.48),(.22,.11,.41,.45)],
        "rime_shelf":[(.09,.22,.61,.62),(.47,.12,.45,.56),(.55,.43,.30,.44)],
        "ruin":[(.12,.15,.68,.66),(.49,.41,.38,.46),(.08,.46,.46,.42)],
        "room":[(.08,.18,.67,.67),(.49,.32,.42,.52),(.23,.10,.38,.46)],
        "lode_lens":[(.12,.23,.71,.56),(.35,.10,.49,.52),(.08,.40,.43,.45)],
        "crystal_geode":[(.17,.18,.61,.64),(.42,.33,.42,.48),(.11,.41,.40,.41)],
        "quartz_geode":[(.15,.18,.66,.63),(.38,.10,.43,.45),(.09,.43,.49,.39)],
        "salt_basin":[(.06,.26,.71,.58),(.45,.17,.47,.51),(.19,.12,.43,.47)],
        "mine_gallery":[(.08,.20,.64,.66),(.49,.38,.42,.47),(.35,.13,.43,.51)],
        "vent":[(.28,.07,.45,.73),(.14,.44,.47,.47),(.43,.22,.39,.42)],
        "fumarole_cluster":[(.17,.19,.61,.65),(.43,.08,.39,.46),(.09,.48,.45,.41),(.53,.48,.36,.37)],
        "oil_reservoir":[(.08,.25,.68,.60),(.48,.18,.43,.56),(.20,.11,.40,.48)],
        "oil_channels":[(.08,.28,.70,.46),(.48,.12,.43,.52),(.19,.46,.45,.42)],
        "magma_vault":[(.13,.18,.70,.63),(.43,.38,.45,.46),(.08,.42,.40,.44)],
        "ember_fracture":[(.20,.09,.51,.69),(.41,.34,.44,.53),(.11,.53,.43,.37)],
    }
    for key,host,w,h,style,resource in specs:
        # Cave rules and worms already respect the whole reservation. Retain
        # solid terrain around these irregular painted shells and chamber lobes.
        ops=[ellipse("paint",1,1,w-2,h-2,host)]
        if style=="geode":
            ops += [ellipse("paint",3,4,w-7,h-8,resource),
                    ellipse("paint",8,7,w-17,h-16,"diamond" if key=="crystal_geode" else "glass")]
        for nx,ny,nw,nh in lobes[key]:
            x=max(4,round(nx*w)); y=max(4,round(ny*h))
            width=min(round(nw*w),w-4-x); height=min(round(nh*h),h-4-y)
            ops.append(ellipse("carve",x,y,width,height))
        if style!="geode":
            # A localized ore patch in the lower wall, rather than a ruler-straight
            # identical ledge across every pocket. It cannot cover the anchor.
            ops.append(ellipse("paint",w//5,3,w//3,4,resource))
        feature(key,key.replace("_"," ").capitalize(),w,h,ops)
    # Local traversal rooms remain independent; shipped profiles omit global straight links.
    for key,host,w,h,outline in [
        ("hearth_promenade","sandstone",108,45,[(5,9,72,29),(51,5,49,32)]),
        ("karst_arcade","limestone",126,60,[(7,8,79,45),(55,19,64,35),(39,5,46,37)]),
        ("mire_gallery","clay",114,42,[(5,11,79,25),(57,5,49,30)])]:
        feature(key,key.replace("_"," ").capitalize(),w,h,
                [ellipse("paint",0,0,w,h,host)]+[ellipse("carve",*lobe) for lobe in outline],"traversal_room")
    lines=["schema_version = 2", "# Sealed reservations exclude traversal connections; walls are at least three fine samples."]
    for key,f in FEATURES.items():
        p="features."+key
        table(lines,p,dict(display_name=f["label"],allow_horizontal_reflection=key!="landing",
              reservation_min_x_fine=0,reservation_min_y_fine=0,reservation_max_x_fine=f["w"],reservation_max_y_fine=f["h"]))
        for i,op in enumerate(f["ops"]): table(lines,p+f".operations.op_{i:02}",dict(order=(i+1)*10,**op))
        if key=="landing":
            for name,role,x,y in [("spawn","start",45,10),("stone","resource_anchor",10,5),("ore","resource_anchor",79,5)]:
                table(lines,p+".markers."+name,dict(role=role,x_fine=x,y_fine=y))
        else:
            x,y=f["anchor"]
            table(lines,p+".markers.anchor",dict(role=f["role"],x_fine=x,y_fine=y))
    save("features.toml",lines)


# Every particle appears in at least one shipped placement+fill recipe.
POCKETS={
    "hearth":[("pocket",[("water",3.0),("oxygen",None)]),("hearth_shelf",[("oxygen",None)])],
    "verdant":[("verdant_bulb",[("water",2.0),("oxygen",None)]),("verdant_root",[("polluted_oxygen",None)])],
    "mire":[("mire_pool",[("polluted_water",4.0),("polluted_oxygen",None)]),("mire_chimney",[("polluted_oxygen",None)])],
    "dunes":[("dune_vault",[("regolith",2.0),("carbon_dioxide",None)]),("dune_bed",[("sand",3.0),("carbon_dioxide",None)])],
    "rime":[("rime_shelf",[("snow",2.0),("carbon_dioxide",None)]),("ruin",[("brine",2.0),("carbon_dioxide",None)])],
    "lode":[("room",[("pebbles",2.0)]),("lode_lens",[("hydrogen",None)])],
    "karst":[("crystal_geode",[("water",3.0),("oxygen",None)]),("quartz_geode",[("water",2.0),("oxygen",None)])],
    "saltworks":[("salt_basin",[("brine",4.0),("carbon_dioxide",None)]),("mine_gallery",[("sand",2.0),("carbon_dioxide",None)])],
    "fumaroles":[("vent",[("steam",None)]),("fumarole_cluster",[("chlorine",None)])],
    "petroleum":[("oil_reservoir",[("crude_oil",4.0),("natural_gas",None)]),("oil_channels",[("crude_oil",2.0),("carbon_dioxide",None)])],
    "ember":[("magma_vault",[("magma",4.0)]),("ember_fracture",[("ash",2.0),("steam",None)])],
}


def worlds():
    lines=["schema_version = 2", "# Frontier has an upper habitable belt; Hollows has deeper broad cave provinces."]
    for profile in ("frontier","hollows"):
        hollow=profile=="hollows"
        p="worlds."+profile
        table(lines,p,dict(display_name="The wide hollows" if hollow else "Frontier basin",background_stack=profile,
              default_wall_material="masonry",boundary_fine=3,min_sites=40 if hollow else 32,max_sites=48 if hollow else 40,
              site_attempts=60,site_spacing_m=140.0 if hollow else 160.0,boundary_warp_amplitude_m=29.0 if hollow else 21.0,
              boundary_warp_x_field="warp_x",boundary_warp_y_field="warp_y",start_biome="hearth",start_feature="landing",
              start_marker="spawn",start_x_fine=2000,start_y_fine=3000,start_clearance_m=3.0,start_route_width_m=1.0))
        for name,marker,mat in [("stone","stone","sandstone"),("ore","ore","copper")]:
            table(lines,p+".start_resources."+name,dict(marker=marker,material=mat,minimum_fine_cells=27,max_distance_m=20.0))
        for i,b in enumerate(BIOMES):
            key=b[0]; lo,hi=b[-2]; dist=b[-1]
            weight={"hearth":4.0,"verdant":2.7,"mire":1.8,"dunes":2.5,"rime":2.5,"lode":3.0,"karst":2.0,"saltworks":1.8,"fumaroles":1.3,"petroleum":1.7,"ember":1.8}[key]
            if hollow:
                weight={"karst":5.0,"mire":4.0,"saltworks":3.5,"lode":.75}.get(key,weight*.65)
                if key in ("karst","mire","saltworks"): lo,hi=.12,.82
            table(lines,p+".biomes."+key,dict(biome=key,weight=weight,required_count=2 if key in (("karst","mire","saltworks") if hollow else ("hearth","lode")) else 1,
                  depth_min_normalized=lo,depth_max_normalized=hi,distance_min_m=float(dist),distance_max_m=2048.0))
            # Each province retains its cave field; chamber scale and warped
            # anisotropy vary without turning an entire region into parallel slits.
            threshold=b[-3] - (.14 if hollow and key!="lode" else 0)
            carve_min,carve_max=round(threshold,3),1.0
            table(lines,p+".rules.caves_"+key,dict(kind="threshold_cave",phase="carve",order=10+i,biome=key,field=b[-4],
                  depth_min_normalized=0.0,depth_max_normalized=1.0,threshold_min=carve_min,threshold_max=carve_max))
            table(lines,p+".rules.tunnels_"+key,dict(kind="carve_worm",phase="carve",order=30+i,biome=key,
                  count=14 if hollow else (3 if key=="lode" else 7),depth_min_normalized=0.0,depth_max_normalized=1.0,
                  length_min_m=25.0 if hollow else 12.0,length_max_m=105.0 if hollow else 50.0,
                  radius_min_m=1.6 if hollow else .8,radius_max_m=3.5 if hollow else 1.8,turn_probability=.30 if key in ("verdant","mire") else .12))
        for i,mat in enumerate(("copper","iron","aluminum","wolframite")):
            table(lines,p+".rules.veins_"+mat,dict(kind="vein_worm",phase="deposit",order=10+i,biome="lode",count=4 if hollow else 10,
                  depth_min_normalized=0.0,depth_max_normalized=1.0,length_min_m=24.0,length_max_m=90.0,radius_min_m=.4,radius_max_m=1.0,
                  turn_probability=.08,material=mat,host_material="granite"))
        # Natural profiles use curved worms and local galleries, without a world-spanning straight-edge graph.
        for i,(biome,feat) in enumerate((("hearth","hearth_promenade"),("karst","karst_arcade"),("mire","mire_gallery"))):
            table(lines,p+".rules.traversal_"+biome,dict(kind="feature",phase="stamp",order=70+i,feature=feat,biome=biome,
                  depth_min_normalized=0.0,depth_max_normalized=1.0,spacing_m=45.0,min_count=1,max_count=5 if hollow else 2,
                  attempts=4096,priority=50-i,reflect=True))
        fill_order=10
        for i,(biome,entries) in enumerate(POCKETS.items()):
            for variant,(feat,fills) in enumerate(entries):
                rule="cisterns" if feat=="pocket" else "landmark_"+feat
                table(lines,p+".rules."+rule,dict(kind="feature",phase="stamp",order=10+i*2+variant,feature=feat,biome=biome,
                      depth_min_normalized=0.0,depth_max_normalized=1.0,spacing_m=35.0,min_count=1,max_count=4 if hollow else 3,
                      attempts=4096,priority=100-i*2-variant,reflect=True))
                for mat,height in fills:
                    density=next(x[3] for x in PARTICLES if x[0]==mat)
                    temp=next(b[3] for b in BIOMES if b[0]==biome)
                    vals=dict(kind="matter_fill",phase="fill",order=fill_order,feature_rule=rule,marker="anchor",material=mat,
                              temperature_k=float(temp),bulk_density_kg_m3=float(density))
                    if height is not None:
                        # Generator heights are reservation-relative, not floor-relative.
                        # Add the carved floor offset so shallow beds are never empty.
                        floor_fine=min(op["y_fine"] for op in FEATURES[feat]["ops"] if op["action"]=="carve")
                        # Divide the canonical integer once, avoiding nonintegral
                        # round-off from adding two independently rounded thirds.
                        vals["fill_height_m"]=(round(height*3)+floor_fine)/3.0
                    table(lines,p+".rules.fill_"+feat+"_"+mat,vals)
                    fill_order+=1
    save("worlds.toml",lines)


def backgrounds():
    lines=["schema_version = 2"]
    for profile in ("frontier","hollows"):
        for order,(name,factor,period) in enumerate(zip(("distant","caverns","formations","backwall"),(.08,.22,.45,.70),(256.0,180.0,112.0,64.0))):
            table(lines,f"backgrounds.{profile}.layers.{name}",dict(order=order,texture=f"textures/background_{profile}_{name}.png",
                  color_rgba="FFFFFFFF",repeat_period_m=period,parallax_factor=factor))
    save("backgrounds.toml",lines)
    save("pack.toml",["schema_version = 2",'pack_id = "ppr_frontier"',"", "[files]",
         'materials = "materials.toml"','biomes = "biomes.toml"','fields = "fields.toml"',
         'features = "features.toml"','worlds = "worlds.toml"','backgrounds = "backgrounds.toml"'])


def hash01(x,y,seed):
    n=(x*374761393+y*668265263+seed*1442695041)&0xffffffff
    n=((n^(n>>13))*1274126177)&0xffffffff
    return (n^(n>>16))/4294967295


def noise(u,v,scale,seed,scale_y=None):
    scale_y=scale if scale_y is None else scale_y
    x=u*scale; y=v*scale_y
    ix=math.floor(x); iy=math.floor(y)
    fx=x-ix; fy=y-iy
    fx=fx*fx*(3-2*fx); fy=fy*fy*(3-2*fy)
    a=hash01(ix%scale,iy%scale_y,seed); b=hash01((ix+1)%scale,iy%scale_y,seed)
    c=hash01(ix%scale,(iy+1)%scale_y,seed); d=hash01((ix+1)%scale,(iy+1)%scale_y,seed)
    return (a+(b-a)*fx)*(1-fy)+(c+(d-c)*fx)*fy


def cells(u,v,scale,seed):
    """Periodic jittered cells: first distance, edge gap and irregular facet plane."""
    x=u*scale; y=v*scale
    ix=math.floor(x); iy=math.floor(y)
    nearest=second=100.0; facet=.5
    for dy in (-1,0,1):
        for dx in (-1,0,1):
            cx=ix+dx; cy=iy+dy; mx=cx%scale; my=cy%scale
            px=cx+.12+.76*hash01(mx,my,seed); py=cy+.12+.76*hash01(mx,my,seed+1)
            lx=x-px; ly=y-py; distance=lx*lx+ly*ly
            if distance<nearest:
                second=nearest; nearest=distance
                facet=.30+.40*hash01(mx,my,seed+2)+lx*(hash01(mx,my,seed+3)-.5)*.55+ly*(hash01(mx,my,seed+4)-.5)*.55
            elif distance<second: second=distance
    return math.sqrt(nearest),math.sqrt(second)-math.sqrt(nearest),facet


def height(u,v,family,seed,key):
    # Periodic low-frequency coordinate displacement breaks lattice regularity
    # without seams or repeated sine ribbons. Every family uses its own seed.
    wu=u+.13*(noise(u,v,3,seed+41)-.5)
    wv=v+.13*(noise(u,v,3,seed+47)-.5)
    broad=noise(wu,wv,4,seed); medium=noise(wu,wv,16,seed+1); grain=noise(wu,wv,64,seed+2)
    if family=="sediment":
        if key=="clay": return .30+.42*broad+.16*medium+.06*grain+.06*noise(wu,wv,6,seed+3,11)
        if key=="shale":
            _,gap,_=cells(wu,wv,6,seed+5)
            return .25+.26*noise(wu,wv,3,seed+3,27)+.20*broad+.12*medium+.08*grain-.10*math.exp(-gap*45)
        strata=noise(wu,wv,3,seed+3,14 if key=="sandstone" else 9)
        lamina=noise(wu,wv+.018*medium,5,seed+4,31)
        return .23+.27*strata+.19*broad+.13*lamina+.12*grain+.06*medium
    if family=="organic": return .23+.43*broad+.23*medium+.09*noise(wu,wv,9,seed+3)+.04*grain
    if family=="granular": return .23+.23*broad+.20*medium+.31*grain
    if family=="crystal":
        scale=5 if key in ("clear_quartz","obsidian") else 9 if key=="diamond" else 7
        _,gap,facet=cells(wu,wv,scale,seed+5)
        return .17+.60*facet+.20*broad+.025*grain-.13*math.exp(-gap*55)
    if family=="metal":
        _,gap,facet=cells(wu,wv,11,seed+5)
        brushed=noise(wu+.05*medium,wv,9,seed+3,51)
        return .29+.24*broad+.16*medium+.19*facet+.10*brushed+.025*grain-.04*math.exp(-gap*60)
    if family=="ore":
        distance,_,facet=cells(wu,wv,9,seed+5)
        nodule=max(0.,1-distance*3.4)
        return .22+.23*broad+.20*medium+.20*nodule+.10*facet+.08*grain
    if family=="ice":
        _,gap,_=cells(wu,wv,5,seed+5)
        veins=math.exp(-gap*38)
        return .36+.29*broad+.16*medium+.05*grain-.16*veins
    if family=="liquid": return .32+.31*noise(wu,wv,4,seed+3,11)+.18*noise(wu,wv,13,seed+4,3)+.15*broad+.04*medium
    if family=="gas": return .35+.40*broad+.18*medium
    if family=="masonry":
        course=int(v*4); x=(u*4+(course%2)*.5)%1; y=(v*4)%1
        mortar=min(x,1-x,y,1-y)<.045
        return .20 if mortar else .62+.15*broad+.08*grain
    _,gap,facet=cells(wu,wv,6,seed+5)
    return .24+.30*broad+.21*medium+.11*grain+.10*facet-.18*math.exp(-gap*42)


def chunk(kind,data):
    return struct.pack(">I",len(data))+kind+data+struct.pack(">I",zlib.crc32(kind+data))


def png(path,size,rgba,srgb):
    rows=b"".join(b"\0"+rgba[y*size*4:(y+1)*size*4] for y in range(size))
    data=b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR",struct.pack(">IIBBBBB",size,size,8,6,0,0,0))
    if srgb: data+=chunk(b"sRGB",b"\0")
    data+=chunk(b"IDAT",zlib.compress(rows,9))+chunk(b"IEND",b"")
    path.write_bytes(data)


def byte(v): return max(0,min(255,round(v*255)))


def texture(key,family):
    size=128; seed=int.from_bytes(hashlib.sha256(key.encode()).digest()[:4],"little")
    heights=[height(x/size,y/size,family,seed,key) for y in range(size) for x in range(size)]
    albedo=bytearray(); normals=bytearray()
    for y in range(size):
        for x in range(size):
            h=heights[y*size+x]
            # Whitening preserves authored material tints without double-tinting.
            shade=max(.34,min(1.0,.55+.50*h))
            if family=="ore": shade=min(1.,shade+.14*(h>.70))
            albedo.extend((byte(shade),byte(shade),byte(shade),255))
            dx=(heights[y*size+(x+1)%size]-heights[y*size+(x-1)%size])*5
            dy=(heights[((y+1)%size)*size+x]-heights[((y-1)%size)*size+x])*5
            length=math.sqrt(dx*dx+dy*dy+1)
            normals.extend((byte(.5-.5*dx/length),byte(.5-.5*dy/length),byte(.5+.5/length),255))
    png(ROOT/f"textures/{key}.png",size,albedo,True)
    png(ROOT/f"textures/{key}_normal.png",size,normals,False)


def background_texture(profile,layer,order):
    size=512; out=bytearray(); seed=710+order*19+(100 if profile=="hollows" else 0)
    palette=((.20,.24,.31),(.14,.19,.23),(.30,.38,.40),(.42,.45,.42)) if profile=="frontier" else ((.24,.20,.32),(.17,.14,.24),(.38,.29,.42),(.48,.39,.47))
    color=palette[order]
    for y in range(size):
        v=y/size
        for x in range(size):
            u=x/size
            wu=u+.16*(noise(u,v,3,seed+41)-.5)
            wv=v+.18*(noise(u,v,3,seed+47)-.5)
            n=noise(wu,wv,8,seed)
            bands=noise(wu,wv,3,seed+4,21)
            outline=noise(wu,wv,7,seed+5,3)
            rib=noise(wu,wv,3,seed+6,16)
            if order==0: alpha=1.; shade=.30+.70*bands+.22*n
            elif order==1:
                bank=noise(wu,wv,4,seed+7)
                alpha=.80 if bank>.48 else 0.; shade=.48+.35*n+.20*bands
            elif order==2: alpha=.62 if n>.58 or rib<.20+.08*outline else 0.; shade=.48+.42*bands+.25*n
            else: alpha=.45 if bands>.73 or n>.75 else 0.; shade=.55+.40*n
            out.extend((byte(color[0]*shade),byte(color[1]*shade),byte(color[2]*shade),byte(alpha)))
    png(ROOT/f"textures/background_{profile}_{layer}.png",size,out,True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-only",action="store_true",help="Rebuild recipes without regenerating unchanged textures")
    args=parser.parse_args()
    (ROOT/"textures").mkdir(parents=True,exist_ok=True)
    materials(); fields(); biomes(); author_features(); worlds(); backgrounds()
    if not args.data_only:
        for key,_,_,_,family,*_ in SOLIDS: texture(key,family)
        for key,_,_,_,_,family,*_ in PARTICLES: texture(key,family)
        for profile in ("frontier","hollows"):
            for order,layer in enumerate(("distant","caverns","formations","backwall")): background_texture(profile,layer,order)
    for path in ROOT.glob("*.toml"):
        with path.open("rb") as stream: tomllib.load(stream)
    print("Generated 45 materials, 11 biomes, 28 fields, 26 templates, two profiles; TOML syntax valid." + ("" if args.data_only else " Generated 98 PNG assets."))


if __name__=="__main__": main()

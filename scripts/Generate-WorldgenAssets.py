"""Maintain procedural textures for PPR's canonical v3 shipped pack.

Simulation and appearance TOML are authored source. This tool reads them without
rewriting recipes or assigning IDs. Existing textures are preserved unless
--overwrite-textures is requested. All material patterns are periodic in both
axes; normal maps derive from the albedo height field. PNG row zero is world Y
zero. Python 3.11+, standard library only; run from any working directory.
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

# Procedural art families are keyed by semantic material name, independently of
# compileDefinition's runtime material bindings.
MATERIAL_FAMILIES = {
    'algae': 'organic',
    'aluminum': 'metal',
    'anhydrite': 'massive',
    'ash': 'granular',
    'autunite': 'platy',
    'azurite': 'botryoidal',
    'basalt': 'rock',
    'bedrock': 'folded',
    'breccia': 'clastic',
    'brine': 'liquid',
    'calcite': 'carbonate',
    'carbon_dioxide': 'gas',
    'chalk': 'chalk',
    'chlorine': 'gas',
    'clay': 'sediment',
    'clear_quartz': 'crystal',
    'coal': 'rock',
    'conglomerate': 'clastic',
    'copper': 'metal',
    'copper_ore': 'ore',
    'crude_oil': 'liquid',
    'crystal': 'crystal',
    'diamond': 'crystal',
    'dirt': 'organic',
    'dolomite': 'carbonate',
    'emerald': 'gem',
    'fluorite': 'cubic',
    'glass': 'ice',
    'gneiss': 'folded',
    'granite': 'rock',
    'gypsum': 'bladed',
    'hydrogen': 'gas',
    'ice': 'ice',
    'iron': 'metal',
    'iron_ore': 'ore',
    'laterite': 'pisolitic',
    'limestone': 'sediment',
    'magma': 'liquid',
    'masonry': 'masonry',
    'natural_gas': 'gas',
    'obsidian': 'crystal',
    'oxygen': 'gas',
    'pebbles': 'rock',
    'phosphorite': 'granular',
    'polluted_ice': 'ice',
    'polluted_oxygen': 'gas',
    'polluted_water': 'liquid',
    'pumice': 'vesicular',
    'regolith': 'granular',
    'ruby': 'gem',
    'rust': 'granular',
    'salt': 'crystal',
    'sand': 'granular',
    'sandstone': 'sediment',
    'shale': 'sediment',
    'slime': 'organic',
    'snow': 'granular',
    'steam': 'gas',
    'sulfur': 'granular',
    'tuff': 'pyroclastic',
    'uranium_ore': 'mineralized',
    'water': 'liquid',
    'wolframite': 'metal',
}


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


GEOLOGICAL_FAMILIES={"carbonate","chalk","bladed","massive","clastic","folded",
                     "vesicular","pisolitic","gem","cubic","botryoidal",
                     "mineralized","platy","pyroclastic"}


def mineral_plates(seed,kind):
    """Unequal overlapping crystals, with local alignment inside growth clusters."""
    result=[]
    for i in range(64 if kind=="platy" else 36):
        cluster=i%5
        cx=hash01(cluster,0,seed+72); cy=hash01(cluster,1,seed+72)
        x=(cx+(hash01(i,0,seed+73)-.5)*.56)%1
        y=(cy+(hash01(i,1,seed+73)-.5)*.56)%1
        angle=(hash01(cluster,2,seed+72)-.5)*2.4+(hash01(i,2,seed+73)-.5)*.42
        width=.028+.065*hash01(i,3,seed+73)
        length=width*(.36+.45*hash01(i,4,seed+73)) if kind=="platy" else width*(.70+.50*hash01(i,4,seed+73))
        result.append((x,y,width,length,math.cos(angle),math.sin(angle),hash01(i,5,seed+73)))
    return result


def geological_sample(u,v,family,seed,key,plates=()):
    """Periodic relief and albedo multipliers; the material supplies its base tint.

    Shape, mineral distribution and color share coordinates so dark pores recede,
    clasts stand above cement, and mineral facets agree with their normal maps.
    """
    broad=noise(u,v,4,seed)
    grain=noise(u,v,64,seed+2)
    wu=u+.055*(noise(u,v,3,seed+41)-.5)
    wv=v+.055*(noise(u,v,3,seed+47)-.5)
    distance,gap,facet=cells(wu,wv,8,seed+5)
    shade=.72; color=(1.,1.,1.)
    if family=="carbonate":
        # Rhombohedral cleavage, coarser and more glassy for calcite.
        _,edge,plane=cells(wu+.08*math.sin(math.tau*wv),wv,6 if key=="calcite" else 12,seed+7)
        cleavage=math.exp(-edge*48)
        h=.32+.47*plane+.12*broad-.16*cleavage
        shade=.65+.40*plane-.22*cleavage
        color=(1.,.98,.92) if key=="calcite" else (.96,.94+.06*broad,1.)
    elif family=="chalk":
        pore,_,_=cells(wu,wv,23,seed+7)
        pit=math.exp(-pore*pore*100)*(.3+.7*noise(wu,wv,7,seed+8))
        bedding=noise(wu,wv,3,seed+9,17)
        h=.57+.08*broad+.06*grain+.06*bedding-.16*pit
        shade=.91+.05*bedding-.20*pit
    elif family=="bladed":
        _,edge,plane=cells(wu*3,wv,7,seed+7)
        fibers=noise(wu,wv,47,seed+8,4)
        joint=math.exp(-edge*65)
        h=.34+.35*plane+.12*fibers-.12*joint
        shade=.71+.24*plane+.10*fibers-.17*joint
        color=(1.,.98,.92+.08*fibers)
    elif family=="massive":
        fracture=math.exp(-gap*55)
        h=.41+.21*facet+.12*noise(wu,wv,26,seed+9)+.08*grain-.17*fracture
        shade=.76+.16*broad+.10*facet-.19*fracture
        color=(.93+.07*broad,.96,1.)
    elif family=="clastic":
        # Rounded pebbles retain cement between them; broken angular clasts
        # occupy interlocking polygons, with a different fracture relief.
        if key=="conglomerate":
            distance,_,facet=cells(wu,wv,6,seed+7)
            radius=.35+.38*facet
            clast=max(0.,min(1.,(radius-distance)*18))
            dome=max(0.,1-(distance/radius)**2)
            h=.30+.07*grain+clast*(.13+.29*dome)
        else:
            clast=max(0.,min(1.,(gap-.035)*24))
            h=.26+.06*grain+clast*(.16+.37*facet)
        cement=.42+.15*noise(wu,wv,21,seed+9)
        stone=.57+.35*facet+.07*grain
        shade=cement*(1-clast)+stone*clast
        # Individual clasts vary in warm/cool lithology, rather than confetti.
        lithology=1. if facet>.56 else 0.
        color=(1.-clast*.18*lithology,1.-clast*.09,1.-clast*.22*(1-lithology))
    elif family=="folded":
        folded=wv+.09*math.sin(math.tau*u)+.10*(noise(u,v,3,seed+9)-.5)
        bands=noise(wu,folded,3,seed+8,23)
        fine=noise(wu,folded,5,seed+10,59)
        seam=max(0.,1-abs(bands-.72)*35)
        h=.30+.31*bands+.13*fine+.07*grain+.05*seam
        shade=.46+.47*bands+.13*fine
        if key=="bedrock":
            # Compressed, folded basement: pale hairline seams in dark grain.
            shade=.49+.31*bands+.12*fine+.11*seam
            h=.27+.32*bands+.16*fine+.05*grain
            color=(.88+.12*seam,.93+.07*seam,1.)
        else:
            color=(.93+.07*bands,.93+.07*bands,1.)
    elif family=="vesicular":
        pore,_,_=cells(wu*2,wv,13,seed+7)
        cavity=math.exp(-pore*pore*35)
        rim=math.exp(-((pore-.29)*13)**2)
        h=.58+.09*broad+.07*grain-.38*cavity+.06*rim
        shade=.81+.10*broad-.43*cavity+.07*rim
    elif family=="pisolitic":
        nodule=max(0.,1-distance*2.1)
        shell=.5+.5*math.cos(distance*37)
        weathering=noise(wu,wv,19,seed+8)
        h=.33+.20*nodule+.07*shell*nodule+.16*grain+.09*weathering
        shade=.56+.18*nodule+.13*broad+.13*weathering+.07*shell*nodule
        color=(1.,.78+.22*nodule,.68+.26*nodule)
    elif family=="gem":
        # Elongated emerald columns and compact ruby facets have deliberately
        # different relief, inclusions and cleavage, not just different tints.
        cu,cv=(wu*2,wv) if key=="emerald" else (wu,wv)
        radius,edge,plane=cells(cu,cv,6,seed+7)
        cleavage=math.exp(-edge*65)
        inclusion=max(0.,noise(cu,cv,17,seed+8)-.68)*1.2
        facet_step=math.floor(plane*7)/7
        h=.26+.53*facet_step+.06*broad-.15*cleavage-.08*inclusion
        shade=.46+.48*facet_step+.10*broad-.24*cleavage-inclusion
        if key=="ruby":
            color=(1.,.65+.32*plane,.77+.23*plane)
        else:
            striation=noise(wu,wv,43,seed+9,3)
            h+=.05*striation
            color=(.62+.32*plane,1.,.78+.22*striation)
    elif family in ("cubic","platy"):
        h=.22+.12*broad+.06*grain
        shade=.36+.16*broad
        rgb=(shade*.83,shade,shade*.65) if family=="platy" else (shade*.88,shade*.77,shade)
        for px,py,width,length,cs,sn,variation in plates:
            dx=(wu-px+.5)%1-.5; dy=(wv-py+.5)%1-.5
            local_x=abs(dx*cs+dy*sn)/width
            local_y=abs(-dx*sn+dy*cs)/length
            edge=1-max(local_x,local_y)
            if edge<=0: continue
            rim=min(1.,edge*22)
            terrace=math.floor(edge*9)/9
            relief=.34+.25*variation+.11*terrace
            if relief<=h: continue
            h=relief-.06*(1-rim)
            facet=.70+.18*(1-local_x)+.08*variation
            if family=="platy":
                rgb=(facet*(.83+.12*variation),facet,facet*(.30+.20*variation))
            else:
                zone=noise(px,py,4,seed+8)
                color=(.43,.95,1.) if zone>.54 else (1.,.59+.20*variation,1.)
                rgb=tuple(facet*c for c in color)
            rgb=tuple(c*(.73+.27*rim) for c in rgb)
        return h,rgb
    elif family=="botryoidal":
        radius,_,plane=cells(wu,wv,14,seed+7)
        dome=math.sqrt(max(0.,1-(radius/.55)**2))
        crust=noise(wu,wv,5,seed+8)
        h=.27+.39*dome+.08*grain+.08*crust
        shade=.44+.36*dome+.17*crust
        color=(.60+.30*plane,.72+.28*dome,1.)
    elif family=="mineralized":
        fracture=math.exp(-gap*45)
        mineral=max(0.,min(1.,(noise(wu,wv,9,seed+8)-.61)*13))
        fleck=max(0.,min(1.,(noise(wu,wv,39,seed+9)-.60)*8))*mineral
        h=.33+.17*broad+.11*facet+.07*grain-.11*fracture+.14*fleck
        # Dark host with sparse green/gold mineralization. No emissive glow.
        shade=.22+.15*broad+.08*facet
        host=(shade*.85,shade*.93,shade)
        gold=1. if noise(wu,wv,11,seed+10)>.52 else 0.
        mineral_color=(.43+.40*gold,.79,.14+.07*gold)
        return h,tuple(a*(1-fleck)+b*fleck for a,b in zip(host,mineral_color))
    else:  # Welded tuff: flattened pumice fragments in fine volcanic ash.
        radius,_,plane=cells(wu,wv*3,9,seed+7)
        fragment=max(0.,min(1.,(.38-radius)*17))
        lamina=noise(wu,wv,3,seed+8,29)
        h=.35+.11*lamina+.10*grain+fragment*(.09+.14*plane)
        shade=.59+.17*lamina+.10*grain+fragment*(.10+.10*plane)
        color=(1.,.94+.06*fragment,.83+.14*fragment)
    return h,tuple(shade*c for c in color)


def texture(key,family,overwrite=False):
    albedo_path=ROOT/f"textures/{key}.png"
    normal_path=ROOT/f"textures/{key}_normal.png"
    write_albedo=overwrite or not albedo_path.exists()
    write_normal=overwrite or not normal_path.exists()
    if not write_albedo and not write_normal: return 0
    size=128; seed=int.from_bytes(hashlib.sha256(key.encode()).digest()[:4],"little")
    geological=family in GEOLOGICAL_FAMILIES
    plates=mineral_plates(seed,family) if family in ("cubic","platy") else ()
    samples=[geological_sample(x/size,y/size,family,seed,key,plates) for y in range(size) for x in range(size)] if geological else []
    heights=[sample[0] for sample in samples] if geological else [height(x/size,y/size,family,seed,key) for y in range(size) for x in range(size)]
    albedo=bytearray(); normals=bytearray()
    for y in range(size):
        for x in range(size):
            h=heights[y*size+x]
            # Whitening preserves authored material tints without double-tinting.
            shade=max(.34,min(1.0,.55+.50*h))
            if family=="ore": shade=min(1.,shade+.14*(h>.70))
            rgb=samples[y*size+x][1] if geological else (shade,shade,shade)
            albedo.extend((*map(byte,rgb),255))
            dx=(heights[y*size+(x+1)%size]-heights[y*size+(x-1)%size])*5
            dy=(heights[((y+1)%size)*size+x]-heights[((y-1)%size)*size+x])*5
            length=math.sqrt(dx*dx+dy*dy+1)
            normals.extend((byte(.5-.5*dx/length),byte(.5-.5*dy/length),byte(.5+.5/length),255))
    # A missing partner must not replace an existing hand-authored map.
    if write_albedo: png(albedo_path,size,albedo,True)
    if write_normal: png(normal_path,size,normals,False)
    return int(write_albedo)+int(write_normal)


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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--material", action="append", default=[], metavar="KEY",
                        help="Limit texture generation to this semantic material key; repeat as needed")
    parser.add_argument("--overwrite-textures", action="store_true",
                        help="Explicitly regenerate selected texture maps, including existing maps")
    args = parser.parse_args()
    with (ROOT / "pack.toml").open("rb") as stream:
        manifest = tomllib.load(stream)
    if manifest.get("schema_version") != 1 or "assets" not in manifest:
        parser.error("pack.toml: expected the named descriptor manifest schema 1")
    declarations = manifest["assets"]
    descriptors = {}
    for key, declaration in declarations.items():
        path = (ROOT / declaration["descriptor"]).resolve()
        if not path.is_relative_to(ROOT.resolve()):
            parser.error(f"{key}: descriptor escapes its pack root")
        with path.open("rb") as stream:
            descriptor = tomllib.load(stream)
        if descriptor.get("schema_version") != 1:
            parser.error(f"{path}: expected descriptor schema 1")
        descriptors[key] = descriptor

    def resolve(reference, kind):
        namespace, separator, key = reference.partition(":")
        if not separator:
            key = namespace
        elif namespace != manifest["pack_id"]:
            parser.error("Procedural generation supports this explicit shipped pack only")
        if key not in declarations or declarations[key]["kind"] != kind:
            parser.error(f"{reference}: expected a declared {kind} asset")
        return descriptors[key]

    worlds = [descriptors[key] for key, declaration in declarations.items() if declaration["kind"] == "sim.world"]
    materials = {}
    for world in worlds:
        for alias, binding in world["palette"]["materials"].items():
            materials.setdefault(alias, []).append(resolve(binding["asset"], "sim.material")["appearance"])
    known = set(MATERIAL_FAMILIES) & materials.keys()
    unknown = set(args.material) - known
    if unknown:
        parser.error("Unknown procedural material keys: " + ", ".join(sorted(unknown)))

    (ROOT / "textures").mkdir(parents=True, exist_ok=True)
    generated = 0
    for key in sorted(known):
        if args.material and key not in args.material:
            continue
        for visual in materials[key]:
            albedo = resolve(visual["albedo_texture"], "image.texture")
            normal = resolve(visual["normal_texture"], "image.texture")
            if albedo["source"] != f"textures/{key}.png" or normal["source"] != f"textures/{key}_normal.png":
                parser.error(f"{key}: procedural generation requires its shipped texture source paths")
        generated += texture(key, MATERIAL_FAMILIES[key], args.overwrite_textures)
    if not args.material:
        for world in worlds:
            profile = world["semantic_key"]
            stack = resolve(world["appearance"]["background_stack"], "colony.background")["definition"]
            for layer, visual in stack["layers"].items():
                source = resolve(visual["texture"], "image.texture")["source"]
                if source != f"textures/background_{profile}_{layer}.png":
                    parser.error(f"{profile}.{layer}: procedural generation requires its shipped texture path")
                path = ROOT / source
                if args.overwrite_textures or not path.exists():
                    background_texture(profile, layer, visual["order"])
                    generated += 1
    print(f"Generated {generated} PNG assets; canonical authored documents preserved; TOML syntax valid.")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Generate small binary STL test models for stlview: cube.stl and torus.stl."""
import struct, math

def write_binary_stl(path, triangles):
    """triangles: list of (v0, v1, v2), each v a 3-tuple."""
    with open(path, "wb") as f:
        f.write(b"\0" * 80)                       # 80-byte header
        f.write(struct.pack("<I", len(triangles)))
        for v0, v1, v2 in triangles:
            # face normal
            ux, uy, uz = (v1[i]-v0[i] for i in range(3))
            wx, wy, wz = (v2[i]-v0[i] for i in range(3))
            nx, ny, nz = uy*wz-uz*wy, uz*wx-ux*wz, ux*wy-uy*wx
            l = math.sqrt(nx*nx+ny*ny+nz*nz) or 1.0
            f.write(struct.pack("<3f", nx/l, ny/l, nz/l))
            for v in (v0, v1, v2):
                f.write(struct.pack("<3f", *v))
            f.write(struct.pack("<H", 0))
    print(f"wrote {path} ({len(triangles)} triangles)")

def cube(s=1.0):
    p = [(-s,-s,-s),( s,-s,-s),( s, s,-s),(-s, s,-s),
         (-s,-s, s),( s,-s, s),( s, s, s),(-s, s, s)]
    faces = [(0,1,2,3),(5,4,7,6),(4,0,3,7),(1,5,6,2),(4,5,1,0),(3,2,6,7)]
    tris = []
    for a,b,c,d in faces:
        tris.append((p[a],p[b],p[c]))
        tris.append((p[a],p[c],p[d]))
    return tris

def torus(R=1.0, r=0.4, nu=48, nv=24):
    def pt(i,j):
        u, v = 2*math.pi*i/nu, 2*math.pi*j/nv
        cu, su, cv, sv = math.cos(u), math.sin(u), math.cos(v), math.sin(v)
        return ((R+r*cv)*cu, (R+r*cv)*su, r*sv)
    tris = []
    for i in range(nu):
        for j in range(nv):
            a,b,c,d = pt(i,j), pt(i+1,j), pt(i+1,j+1), pt(i,j+1)
            tris.append((a,b,c)); tris.append((a,c,d))
    return tris

if __name__ == "__main__":
    write_binary_stl("cube.stl", cube())
    write_binary_stl("torus.stl", torus())

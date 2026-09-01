import { describe, expect, it } from 'vitest';

import {
  StructuralGuideTracker,
  detectStructuralGuideGeometry,
  distancePointToSegment,
  matchEdgeToManualPoints,
  type StructuralGuideInput,
} from '../../src/xr/structuralGuide';

interface CloudBuilder {
  positions: number[];
  normals: number[];
  confidence: number[];
}

function builder(): CloudBuilder { return { positions: [], normals: [], confidence: [] }; }
function addPoint(cloud: CloudBuilder, p: readonly [number,number,number], n: readonly [number,number,number], confidence=0.94): void {
  cloud.positions.push(...p); cloud.normals.push(...n); cloud.confidence.push(confidence);
}
function addPlaneX(cloud: CloudBuilder, x=0): void {
  for(let y=-0.22;y<=0.2201;y+=0.035) for(let z=0;z<=0.4201;z+=0.035) addPoint(cloud,[x,y,z],[1,0,0]);
}
function addPlaneY(cloud: CloudBuilder, y=0): void {
  for(let x=-0.22;x<=0.2201;x+=0.035) for(let z=0;z<=0.4201;z+=0.035) addPoint(cloud,[x,y,z],[0,1,0]);
}
function addPlaneZ(cloud: CloudBuilder, z=0): void {
  for(let x=-0.22;x<=0.2201;x+=0.035) for(let y=-0.22;y<=0.2201;y+=0.035) addPoint(cloud,[x,y,z],[0,0,1]);
}
function inputFrom(cloud:CloudBuilder,keyframeCount=10):StructuralGuideInput {
  return {positions:Float32Array.from(cloud.positions),normals:Float32Array.from(cloud.normals),confidence:Float32Array.from(cloud.confidence),keyframeCount,voxelSizeMeters:0.04};
}

describe('structural guide geometry',()=>{
  it('does not invent edges on one flat plane',()=>{
    const cloud=builder();addPlaneX(cloud);
    const result=detectStructuralGuideGeometry(inputFrom(cloud));
    expect(result.planarPatches).toBeGreaterThan(5);
    expect(result.edges).toHaveLength(0);
    expect(result.corners).toHaveLength(0);
  });

  it('finds a persistent two-plane edge',()=>{
    const cloud=builder();addPlaneX(cloud);addPlaneY(cloud);
    const result=detectStructuralGuideGeometry(inputFrom(cloud));
    expect(result.edges.length).toBeGreaterThan(0);
    expect(result.edges[0].geometryConfidence).toBeGreaterThan(0.48);
    expect(Math.abs(result.edges[0].direction[2])).toBeGreaterThan(0.65);
  });

  it('finds a three-plane corner near the true intersection',()=>{
    const cloud=builder();addPlaneX(cloud);addPlaneY(cloud);addPlaneZ(cloud);
    const result=detectStructuralGuideGeometry(inputFrom(cloud));
    expect(result.corners.length).toBeGreaterThan(0);
    const nearest=[...result.corners].sort((a,b)=>Math.hypot(...a.position)-Math.hypot(...b.position))[0];
    expect(Math.hypot(...nearest.position)).toBeLessThan(0.18);
  });

  it('ignores low-confidence geometry',()=>{
    const cloud=builder();
    for(let i=0;i<100;i+=1)addPoint(cloud,[0,(i%10)*0.02,Math.floor(i/10)*0.02],[1,0,0],0.3);
    const result=detectStructuralGuideGeometry(inputFrom(cloud));
    expect(result.analyzedPoints).toBe(100);
    expect(result.planarPatches).toBe(0);
    expect(result.edges).toHaveLength(0);
  });

  it('treats opposite normal signs as the same plane orientation',()=>{
    const cloud=builder();
    for(let y=-0.2;y<=0.2;y+=0.03)for(let z=0;z<=0.3;z+=0.03)addPoint(cloud,[0,y,z],[(Math.round(y*100)%2===0?1:-1),0,0]);
    const result=detectStructuralGuideGeometry(inputFrom(cloud));
    expect(result.planarPatches).toBeGreaterThan(0);
    expect(result.edges).toHaveLength(0);
  });
});

describe('StructuralGuideTracker temporal evidence',()=>{
  it('cannot mature a suggestion by replaying the same keyframe',()=>{
    const cloud=builder();addPlaneX(cloud);addPlaneY(cloud);addPlaneZ(cloud);
    const tracker=new StructuralGuideTracker();
    for(let repeat=0;repeat<8;repeat+=1){
      const result=tracker.update(inputFrom(cloud,10));
      expect(result.corners).toHaveLength(0);
    }
  });

  it('matures geometry only as independent keyframes advance',()=>{
    const cloud=builder();addPlaneX(cloud);addPlaneY(cloud);addPlaneZ(cloud);
    const tracker=new StructuralGuideTracker();
    expect(tracker.update(inputFrom(cloud,10)).corners).toHaveLength(0);
    const result=tracker.update(inputFrom(cloud,12));
    expect(result.corners.length).toBeGreaterThan(0);
    expect(result.corners[0].observations).toBeGreaterThanOrEqual(2);
  });

  it('reset discards temporal confidence',()=>{
    const cloud=builder();addPlaneX(cloud);addPlaneY(cloud);addPlaneZ(cloud);
    const tracker=new StructuralGuideTracker();
    tracker.update(inputFrom(cloud,10));
    expect(tracker.update(inputFrom(cloud,12)).corners.length).toBeGreaterThan(0);
    tracker.reset();
    expect(tracker.update(inputFrom(cloud,30)).corners).toHaveLength(0);
  });
});

describe('edge to operator geometry matching',()=>{
  it('matches two manual points near the same detected edge',()=>{
    const cloud=builder();addPlaneX(cloud);addPlaneY(cloud);
    const tracker=new StructuralGuideTracker();tracker.update(inputFrom(cloud,10));const result=tracker.update(inputFrom(cloud,12));
    expect(result.edges.length).toBeGreaterThan(0);
    const edge=result.edges[0];
    const points=[{id:1,position:edge.start},{id:2,position:edge.end}] as const;
    const match=matchEdgeToManualPoints(edge,points,0.03);
    expect(match?.[0].id).toBe(1);
    expect(match?.[1].id).toBe(2);
  });

  it('rejects unrelated manual points',()=>{
    const cloud=builder();addPlaneX(cloud);addPlaneY(cloud);
    const tracker=new StructuralGuideTracker();tracker.update(inputFrom(cloud,10));const result=tracker.update(inputFrom(cloud,12));
    const edge=result.edges[0];
    const match=matchEdgeToManualPoints(edge,[{id:1,position:[4,4,4]},{id:2,position:[5,5,5]}],0.1);
    expect(match).toBeNull();
  });

  it('computes point-to-segment distance without extending the segment',()=>{
    expect(distancePointToSegment([2,0,0],[0,0,0],[1,0,0])).toBeCloseTo(1,6);
    expect(distancePointToSegment([0.5,0.2,0],[0,0,0],[1,0,0])).toBeCloseTo(0.2,6);
  });
});

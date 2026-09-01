export type ProjectPoint = readonly [number, number, number];
export type ProjectVector = readonly [number, number, number];

export interface StructuralGuideInput {
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  keyframeCount: number;
  voxelSizeMeters: number;
}

export interface StructuralCornerCandidate {
  id: string;
  kind: 'corner';
  position: ProjectPoint;
  confidence: number;
  geometryConfidence: number;
  support: number;
  observations: number;
}

export interface StructuralEdgeCandidate {
  id: string;
  kind: 'edge';
  start: ProjectPoint;
  end: ProjectPoint;
  midpoint: ProjectPoint;
  direction: ProjectVector;
  confidence: number;
  geometryConfidence: number;
  support: number;
  observations: number;
}

export interface StructuralGuideResult {
  corners: readonly StructuralCornerCandidate[];
  edges: readonly StructuralEdgeCandidate[];
  analyzedPoints: number;
  planarPatches: number;
  computeMs: number;
  keyframeCount: number;
}

interface PlanePatch {
  key: string;
  center: ProjectPoint;
  normal: ProjectVector;
  d: number;
  support: number;
  confidence: number;
  ix: number;
  iy: number;
  iz: number;
}

interface RawCorner {
  position: ProjectPoint;
  geometryConfidence: number;
  support: number;
}

interface RawEdge {
  start: ProjectPoint;
  end: ProjectPoint;
  midpoint: ProjectPoint;
  direction: ProjectVector;
  geometryConfidence: number;
  support: number;
}

interface TrackedCorner extends RawCorner { observations: number; lastKeyframe: number; }
interface TrackedEdge extends RawEdge { observations: number; lastKeyframe: number; }

const PATCH_CELL_METERS = 0.10;
const MIN_POINT_CONFIDENCE = 0.68;
const MIN_PATCH_SUPPORT = 3;
const MAX_PATCH_NEIGHBOUR_METERS = 0.24;
const MIN_PLANE_ANGLE_RAD = (34 * Math.PI) / 180;
const MAX_CORNER_CENTER_DISTANCE_METERS = 0.30;
const TRACK_CELL_METERS = 0.12;
const MAX_VISIBLE_CORNERS = 8;
const MAX_VISIBLE_EDGES = 10;

function clamp01(value: number): number { return Math.max(0, Math.min(1, value)); }
function finite3(x: number, y: number, z: number): boolean { return Number.isFinite(x) && Number.isFinite(y) && Number.isFinite(z); }
function dot(a: ProjectVector, b: ProjectVector): number { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
function cross(a: ProjectVector, b: ProjectVector): ProjectVector { return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]; }
function length(v: ProjectVector): number { return Math.hypot(v[0],v[1],v[2]); }
function normalize(v: ProjectVector): ProjectVector | null { const m=length(v); return m>1e-8 ? [v[0]/m,v[1]/m,v[2]/m] : null; }
function distance(a: ProjectPoint,b: ProjectPoint): number { return Math.hypot(a[0]-b[0],a[1]-b[1],a[2]-b[2]); }
function midpoint(a: ProjectPoint,b: ProjectPoint): ProjectPoint { return [(a[0]+b[0])*0.5,(a[1]+b[1])*0.5,(a[2]+b[2])*0.5]; }
function addScaled(p: ProjectPoint, v: ProjectVector, scale: number): ProjectPoint { return [p[0]+v[0]*scale,p[1]+v[1]*scale,p[2]+v[2]*scale]; }
function canonicalNormal(normal: ProjectVector): ProjectVector {
  const abs=[Math.abs(normal[0]),Math.abs(normal[1]),Math.abs(normal[2])];
  let axis=0; if(abs[1]>abs[axis]) axis=1; if(abs[2]>abs[axis]) axis=2;
  return normal[axis] < 0 ? [-normal[0],-normal[1],-normal[2]] : normal;
}
function cellCoord(value:number,size:number):number { return Math.floor(value/size); }
function cellKey(x:number,y:number,z:number,size:number):string { return `${cellCoord(x,size)}:${cellCoord(y,size)}:${cellCoord(z,size)}`; }
function quantizedDirection(v:ProjectVector):string {
  const n=canonicalNormal(v);
  return `${Math.round(n[0]*4)}:${Math.round(n[1]*4)}:${Math.round(n[2]*4)}`;
}

function buildPlanePatches(input: StructuralGuideInput): PlanePatch[] {
  interface Acc { sx:number;sy:number;sz:number;snx:number;sny:number;snz:number;sc:number;count:number;ix:number;iy:number;iz:number; }
  const map=new Map<string,Acc>();
  const count=Math.min(Math.floor(input.positions.length/3),Math.floor(input.normals.length/3),input.confidence.length);
  for(let index=0;index<count;index+=1){
    const confidence=input.confidence[index]; if(confidence<MIN_POINT_CONFIDENCE) continue;
    const o=index*3; const x=input.positions[o],y=input.positions[o+1],z=input.positions[o+2];
    let nx=input.normals[o],ny=input.normals[o+1],nz=input.normals[o+2];
    if(!finite3(x,y,z)||!finite3(nx,ny,nz)) continue;
    const nm=Math.hypot(nx,ny,nz); if(nm<0.5) continue; nx/=nm;ny/=nm;nz/=nm;
    const canonical=canonicalNormal([nx,ny,nz]); nx=canonical[0];ny=canonical[1];nz=canonical[2];
    const ix=cellCoord(x,PATCH_CELL_METERS),iy=cellCoord(y,PATCH_CELL_METERS),iz=cellCoord(z,PATCH_CELL_METERS);
    const key=`${ix}:${iy}:${iz}`; const acc=map.get(key)??{sx:0,sy:0,sz:0,snx:0,sny:0,snz:0,sc:0,count:0,ix,iy,iz};
    // Align normals within a patch before averaging because eigenvectors have sign ambiguity.
    if(acc.count>0 && acc.snx*nx+acc.sny*ny+acc.snz*nz<0){nx=-nx;ny=-ny;nz=-nz;}
    acc.sx+=x;acc.sy+=y;acc.sz+=z;acc.snx+=nx;acc.sny+=ny;acc.snz+=nz;acc.sc+=confidence;acc.count+=1;map.set(key,acc);
  }
  const patches:PlanePatch[]=[];
  for(const [key,acc] of map){
    if(acc.count<MIN_PATCH_SUPPORT) continue;
    const normal=normalize([acc.snx,acc.sny,acc.snz]); if(normal===null) continue;
    const center:[number,number,number]=[acc.sx/acc.count,acc.sy/acc.count,acc.sz/acc.count];
    patches.push({key,center,normal,d:dot(normal,center),support:acc.count,confidence:acc.sc/acc.count,ix:acc.ix,iy:acc.iy,iz:acc.iz});
  }
  return patches;
}

/** Point on two planes closest to anchor. */
function intersectTwoPlanesNear(a:PlanePatch,b:PlanePatch,anchor:ProjectPoint):{point:ProjectPoint;direction:ProjectVector}|null{
  const c=dot(a.normal,b.normal); const det=1-c*c; if(det<1e-4) return null;
  const ra=a.d-dot(a.normal,anchor); const rb=b.d-dot(b.normal,anchor);
  const la=(ra-c*rb)/det; const lb=(rb-c*ra)/det;
  const point:[number,number,number]=[
    anchor[0]+a.normal[0]*la+b.normal[0]*lb,
    anchor[1]+a.normal[1]*la+b.normal[1]*lb,
    anchor[2]+a.normal[2]*la+b.normal[2]*lb,
  ];
  const direction=normalize(cross(a.normal,b.normal));
  return direction===null?null:{point,direction};
}

function solveThreePlanes(a:PlanePatch,b:PlanePatch,c:PlanePatch):ProjectPoint|null{
  const bc=cross(b.normal,c.normal); const ca=cross(c.normal,a.normal); const ab=cross(a.normal,b.normal);
  const determinant=dot(a.normal,bc); if(Math.abs(determinant)<0.12) return null;
  const inv=1/determinant;
  const point:[number,number,number]=[
    (a.d*bc[0]+b.d*ca[0]+c.d*ab[0])*inv,
    (a.d*bc[1]+b.d*ca[1]+c.d*ab[1])*inv,
    (a.d*bc[2]+b.d*ca[2]+c.d*ab[2])*inv,
  ];
  return finite3(point[0],point[1],point[2])?point:null;
}

function geometryScoreForPair(a:PlanePatch,b:PlanePatch,centerDistance:number):number{
  const absDot=Math.min(1,Math.abs(dot(a.normal,b.normal)));
  const angle=Math.acos(absDot); const angleScore=clamp01((angle-MIN_PLANE_ANGLE_RAD)/(Math.PI/2-MIN_PLANE_ANGLE_RAD));
  const supportScore=clamp01((a.support+b.support)/18);
  const quality=(a.confidence+b.confidence)*0.5;
  const proximity=clamp01(1-centerDistance/MAX_PATCH_NEIGHBOUR_METERS);
  return clamp01(angleScore*0.34+supportScore*0.23+quality*0.27+proximity*0.16);
}

function dedupeEdges(raw:RawEdge[]):RawEdge[]{
  const best=new Map<string,RawEdge>();
  for(const edge of raw){
    const key=`${cellKey(edge.midpoint[0],edge.midpoint[1],edge.midpoint[2],TRACK_CELL_METERS)}:${quantizedDirection(edge.direction)}`;
    const current=best.get(key); if(current===undefined||edge.geometryConfidence>current.geometryConfidence) best.set(key,edge);
  }
  return [...best.values()].sort((a,b)=>b.geometryConfidence-a.geometryConfidence);
}
function dedupeCorners(raw:RawCorner[]):RawCorner[]{
  const best=new Map<string,RawCorner>();
  for(const corner of raw){const key=cellKey(corner.position[0],corner.position[1],corner.position[2],TRACK_CELL_METERS);const current=best.get(key);if(current===undefined||corner.geometryConfidence>current.geometryConfidence)best.set(key,corner);}
  return [...best.values()].sort((a,b)=>b.geometryConfidence-a.geometryConfidence);
}

export function detectStructuralGuideGeometry(input:StructuralGuideInput):{corners:RawCorner[];edges:RawEdge[];analyzedPoints:number;planarPatches:number}{
  const patches=buildPlanePatches(input); const edges:RawEdge[]=[]; const cornerSeeds=new Map<number,number[]>();
  for(let i=0;i<patches.length;i+=1){
    const a=patches[i];
    for(let j=i+1;j<patches.length;j+=1){
      const b=patches[j];
      if(Math.abs(a.ix-b.ix)>2||Math.abs(a.iy-b.iy)>2||Math.abs(a.iz-b.iz)>2) continue;
      const centerDistance=distance(a.center,b.center); if(centerDistance>MAX_PATCH_NEIGHBOUR_METERS) continue;
      const angle=Math.acos(Math.min(1,Math.abs(dot(a.normal,b.normal)))); if(angle<MIN_PLANE_ANGLE_RAD) continue;
      const intersection=intersectTwoPlanesNear(a,b,midpoint(a.center,b.center)); if(intersection===null) continue;
      if(distance(intersection.point,a.center)>MAX_CORNER_CENTER_DISTANCE_METERS||distance(intersection.point,b.center)>MAX_CORNER_CENTER_DISTANCE_METERS) continue;
      const score=geometryScoreForPair(a,b,centerDistance); if(score<0.48) continue;
      const halfLength=Math.max(0.09,Math.min(0.34,0.08+0.008*(a.support+b.support)));
      edges.push({start:addScaled(intersection.point,intersection.direction,-halfLength),end:addScaled(intersection.point,intersection.direction,halfLength),midpoint:intersection.point,direction:intersection.direction,geometryConfidence:score,support:a.support+b.support});
      const forA=cornerSeeds.get(i)??[];forA.push(j);cornerSeeds.set(i,forA);
    }
  }

  const corners:RawCorner[]=[];
  for(const [i,neighbours] of cornerSeeds){
    const a=patches[i];
    for(let x=0;x<neighbours.length;x+=1)for(let y=x+1;y<neighbours.length;y+=1){
      const b=patches[neighbours[x]],c=patches[neighbours[y]];
      const angleBC=Math.acos(Math.min(1,Math.abs(dot(b.normal,c.normal)))); if(angleBC<MIN_PLANE_ANGLE_RAD) continue;
      const point=solveThreePlanes(a,b,c); if(point===null) continue;
      const maxDistance=Math.max(distance(point,a.center),distance(point,b.center),distance(point,c.center)); if(maxDistance>MAX_CORNER_CENTER_DISTANCE_METERS) continue;
      const determinant=Math.abs(dot(a.normal,cross(b.normal,c.normal)));
      const support=a.support+b.support+c.support;
      const quality=(a.confidence+b.confidence+c.confidence)/3;
      const score=clamp01(clamp01(determinant/0.75)*0.35+clamp01(support/26)*0.25+quality*0.25+clamp01(1-maxDistance/MAX_CORNER_CENTER_DISTANCE_METERS)*0.15);
      if(score>=0.48) corners.push({position:point,geometryConfidence:score,support});
    }
  }
  return {corners:dedupeCorners(corners),edges:dedupeEdges(edges),analyzedPoints:Math.min(Math.floor(input.positions.length/3),input.confidence.length),planarPatches:patches.length};
}

function trackedConfidence(geometry:number,observations:number):number{
  const persistence=clamp01((observations-1)/4); return clamp01(geometry*0.68+persistence*0.32);
}

/**
 * Temporal gate: looking at the same geometry without new keyframes cannot make
 * an EDGE?/CORNER? suggestion mature by itself.
 */
export class StructuralGuideTracker {
  private corners=new Map<string,TrackedCorner>();
  private edges=new Map<string,TrackedEdge>();

  reset():void{this.corners.clear();this.edges.clear();}

  update(input:StructuralGuideInput,computeMs=0):StructuralGuideResult{
    const raw=detectStructuralGuideGeometry(input);
    const seenCorners=new Set<string>(); const seenEdges=new Set<string>();
    for(const candidate of raw.corners){
      const key=cellKey(candidate.position[0],candidate.position[1],candidate.position[2],TRACK_CELL_METERS); seenCorners.add(key);
      const current=this.corners.get(key);
      if(current===undefined){this.corners.set(key,{...candidate,observations:1,lastKeyframe:input.keyframeCount});}
      else{current.position=candidate.position;current.geometryConfidence=candidate.geometryConfidence;current.support=candidate.support;if(input.keyframeCount-current.lastKeyframe>=2){current.observations+=1;current.lastKeyframe=input.keyframeCount;}}
    }
    for(const candidate of raw.edges){
      const key=`${cellKey(candidate.midpoint[0],candidate.midpoint[1],candidate.midpoint[2],TRACK_CELL_METERS)}:${quantizedDirection(candidate.direction)}`; seenEdges.add(key);
      const current=this.edges.get(key);
      if(current===undefined){this.edges.set(key,{...candidate,observations:1,lastKeyframe:input.keyframeCount});}
      else{current.start=candidate.start;current.end=candidate.end;current.midpoint=candidate.midpoint;current.direction=candidate.direction;current.geometryConfidence=candidate.geometryConfidence;current.support=candidate.support;if(input.keyframeCount-current.lastKeyframe>=2){current.observations+=1;current.lastKeyframe=input.keyframeCount;}}
    }
    // Stale suggestions are forgotten instead of accumulating forever as the operator moves on.
    for(const [key,value] of this.corners) if(!seenCorners.has(key)&&input.keyframeCount-value.lastKeyframe>10)this.corners.delete(key);
    for(const [key,value] of this.edges) if(!seenEdges.has(key)&&input.keyframeCount-value.lastKeyframe>10)this.edges.delete(key);

    const corners:StructuralCornerCandidate[]=[];
    for(const [id,c] of this.corners){const confidence=trackedConfidence(c.geometryConfidence,c.observations);if(c.observations>=2&&confidence>=0.58)corners.push({id:`corner:${id}`,kind:'corner',position:c.position,confidence,geometryConfidence:c.geometryConfidence,support:c.support,observations:c.observations});}
    const edges:StructuralEdgeCandidate[]=[];
    for(const [id,e] of this.edges){const confidence=trackedConfidence(e.geometryConfidence,e.observations);if(e.observations>=2&&confidence>=0.58)edges.push({id:`edge:${id}`,kind:'edge',start:e.start,end:e.end,midpoint:e.midpoint,direction:e.direction,confidence,geometryConfidence:e.geometryConfidence,support:e.support,observations:e.observations});}
    corners.sort((a,b)=>b.confidence-a.confidence);edges.sort((a,b)=>b.confidence-a.confidence);
    return {corners:corners.slice(0,MAX_VISIBLE_CORNERS),edges:edges.slice(0,MAX_VISIBLE_EDGES),analyzedPoints:raw.analyzedPoints,planarPatches:raw.planarPatches,computeMs,keyframeCount:input.keyframeCount};
  }
}

export function distancePointToSegment(point:ProjectPoint,start:ProjectPoint,end:ProjectPoint):number{
  const dx=end[0]-start[0],dy=end[1]-start[1],dz=end[2]-start[2]; const denom=dx*dx+dy*dy+dz*dz;
  if(denom<1e-9)return distance(point,start);
  const t=clamp01(((point[0]-start[0])*dx+(point[1]-start[1])*dy+(point[2]-start[2])*dz)/denom);
  return Math.hypot(point[0]-(start[0]+dx*t),point[1]-(start[1]+dy*t),point[2]-(start[2]+dz*t));
}

export interface ManualPointLike { id:number; position:ProjectPoint; }
export function matchEdgeToManualPoints(edge:StructuralEdgeCandidate,points:readonly ManualPointLike[],radiusMeters=0.14):readonly [ManualPointLike,ManualPointLike]|null{
  if(points.length<2)return null;
  let best:null|{a:ManualPointLike;b:ManualPointLike;score:number}=null;
  for(let i=0;i<points.length;i+=1)for(let j=i+1;j<points.length;j+=1){
    const p=points[i],q=points[j]; const direct=distance(p.position,edge.start)+distance(q.position,edge.end); const reverse=distance(p.position,edge.end)+distance(q.position,edge.start);
    const score=Math.min(direct,reverse); if(score>radiusMeters*2)continue;
    if(distancePointToSegment(midpoint(p.position,q.position),edge.start,edge.end)>radiusMeters)continue;
    if(best===null||score<best.score)best=direct<=reverse?{a:p,b:q,score}:{a:q,b:p,score};
  }
  return best===null?null:[best.a,best.b];
}

#include <cassert>
#include <cmath>
#include <iostream>
#include "cabinet_logic.h"
int main() {
  using namespace Cabinet;
  Settings s; assert(valid(s));
  SequenceGuard seq; bool restarted;
  assert(seq.accept("0123456789abcdef",5,restarted) && restarted);
  assert(!seq.accept("0123456789abcdef",5,restarted));
  assert(!seq.accept("0123456789abcdef",4,restarted));
  assert(seq.accept("0123456789abcdef",6,restarted) && !restarted);
  assert(seq.accept("abcdef0123456789",1,restarted) && restarted);
  assert(!seq.accept("malformed",2,restarted));
  SequenceGuard rollover;
  assert(rollover.accept("0123456789abcdef",UINT32_MAX,restarted));
  assert(rollover.accept("0123456789abcdef",0,restarted) && !restarted);
  auto bad=s; bad.pressAngle=s.restAngle; assert(!valid(bad));
  bad=s; bad.doorThresholdCm=NAN; assert(!valid(bad));
  NoiseGate g;
  assert(!g.sample(95,0,s,2500));
  assert(!g.sample(80,1000,s,2500)); // A short peak is cancelled.
  for (uint32_t t=2000;t<5000;t+=1000) assert(!g.sample(92,t,s,2500));
  assert(g.sample(92,5000,s,2500)); assert(!g.armed);
  for(uint32_t t=6000;t<14000;t+=1000) assert(!g.sample(100,t,s,2500));
  assert(!g.armed); // No repeated presses while loud.
  assert(!g.sample(87,14000,s,2500)); assert(!g.sample(89,15000,s,2500));
  for(uint32_t t=16000;t<=19000;t+=1000) assert(!g.sample(87,t,s,2500));
  assert(g.armed); // 3 s of fresh low readings, below 88 dB.
  assert(!g.sample(91,20000,s,2500));
  assert(!g.sample(91,24000,s,2500)); // A missing-data gap starts over.
  assert(!g.sample(91,25000,s,2500)); g.invalidate();
  assert(!g.sample(91,26000,s,2500)); assert(!g.sample(91,27000,s,2500));
  assert(!g.sample(NAN,28000,s,2500)); assert(!g.fresh);
  NoiseGate wrap;
  for(uint32_t offset=0;offset<3000;offset+=1000) assert(!wrap.sample(90,UINT32_MAX-1000+offset,s,2500));
  assert(wrap.sample(90,UINT32_MAX-1000+3000,s,2500));
  ServoCycle servo; assert(servo.start(100,s)); assert(!servo.start(200,s));
  assert(servo.angle==110 && s.restAngle-s.pressAngle==40);
  servo.tick(350); assert(std::fabs(servo.angle-90)<.01);
  servo.tick(600); assert(servo.phase==Phase::Hold && servo.angle==70);
  servo.tick(1100); assert(servo.phase==Phase::Return);
  servo.tick(1350); assert(std::fabs(servo.angle-90)<.01);
  servo.tick(1600); assert(servo.phase==Phase::Idle && servo.angle==110 && servo.pressCount==1);
  // Independent door detection: median rejects one spike, debounce and hysteresis.
  DoorMonitor door;
  for(uint32_t t=0;t<=1200;t+=100) door.sample(t==500?100:10,t,20,2,600);
  assert(door.state==Door::Closed && door.distance==10);
  for(uint32_t t=1300;t<=2300;t+=100) door.sample(30,t,20,2,600);
  assert(door.state==Door::Open);
  for(uint32_t t=2400;t<=3500;t+=100) door.sample(19,t,20,2,600);
  assert(door.state==Door::Open); // Open remains in the 18–20 cm hysteresis band.
  for(uint32_t t=3600;t<=4600;t+=100) door.sample(17,t,20,2,600);
  assert(door.state==Door::Closed);
  door.sample(NAN,4700,20,2,600); assert(door.state==Door::Unknown && std::isnan(door.distance));
  std::cout << "PASS: peak rejection, sustained trigger, latch/rearm, stale gaps, wraparound, servo return, door filtering/hysteresis/fault\n";
}

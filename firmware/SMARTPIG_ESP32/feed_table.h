#pragma once
// Daily feed per pig (grams).
//   1-35 days   : 500 g   (group table: 1-60 days = 0.5 kg)
//   36-140 days : B-MEG Premium chart below (from Pig-Ratio-per-pig.csv)
//   141-180 days: 2500 g  (group table: last row = 2.5 kg)
// The website (feedingData.js) uses the same numbers. Keep both in step.
#define FEED_AGE_MIN 36
#define FEED_AGE_MAX 140
#define FEED_LOW_AGE_G  500
#define FEED_HIGH_AGE_G 2500
const uint16_t FEED_PER_PIG[105] = {
250,270,285,305,320,340,355,375,395,410,430,445,465,480,500,500,525,550,570,595,620,645,670,695,715,740,765,790,815,840,860,885,910,935,960,985,1005,1030,1055,1080,1105,1130,1150,1175,1200,1200,1230,1260,1290,1320,1345,1375,1405,1435,1465,1495,1525,1555,1580,1610,1640,1670,1700,1730,1760,1790,1820,1845,1875,1905,1935,1965,1995,2025,2055,2080,2110,2140,2170,2200,2200,2210,2225,2240,2250,2260,2275,2290,2300,2310,2325,2340,2350,2360,2375,2390,2400,2410,2425,2440,2450,2460,2475,2490,2500
};
// Stage limits (g/pig/day): 1=Pre-Starter 2=Starter 3=Grower 1 4=Grower 2
const uint16_t FEED_STAGE_MIN[5] = {0,250,500,1200,2200};
const uint16_t FEED_STAGE_MAX[5] = {0,500,1200,2200,2500};

inline int stageOf(int age) { return age <= 50 ? 1 : age <= 80 ? 2 : age <= 115 ? 3 : 4; }

// Base daily feed per pig for any age (no AI adjustment)
inline float dailyPerPig(int age) {
  if (age < FEED_AGE_MIN) return (float)FEED_LOW_AGE_G;
  if (age > FEED_AGE_MAX) return (float)FEED_HIGH_AGE_G;
  return (float)FEED_PER_PIG[age - FEED_AGE_MIN];
}

// The Random Forest was trained on 36-140 days only
inline bool rfCoversAge(int age) { return age >= FEED_AGE_MIN && age <= FEED_AGE_MAX; }

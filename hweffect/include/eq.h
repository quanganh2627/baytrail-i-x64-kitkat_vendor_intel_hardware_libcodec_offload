#define NUM_BANDS 5
#define NUM_PRESETS 10
static const int PRESET_CUSTOM = -1;
static const char* gPresets[NUM_PRESETS] = {
        "Normal",
        "Classical",
        "Dance",
        "Flat",
        "Folk",
        "Heavy Metal",
        "Hip Hop",
        "Jazz",
        "Pop",
        "Rock"};
static const uint32_t bandFreqRange[NUM_BANDS][2] = {
                                       {0, 88000},
                                       {110000, 360000},
                                       {500000, 1300000},
                                       {1800000, 6000000},
                                       {7000000, 19500000}};
static const uint16_t bandCFrequencies[] = {
                                  50,  /* Frequencies in Hz */
                                  200,
                                  800,
                                  3000,
                                  15000};
static const int16_t bandGains[] = {
                     3, 0, 0, 0, 3,       /* Normal Preset */
                     8, 5, -3, 5, 6,      /* Classical Preset */
                     15, -6, 7, 13, 10,   /* Dance Preset */
                     0, 0, 0, 0, 0,       /* Flat Preset */
                     6, -2, -2, 6, -3,    /* Folk Preset */
                     8, -8, 13, -1, -4,   /* Heavy Metal Preset */
                     10, 6, -4, 5, 8,     /* Hip Hop Preset */
                     8, 5, -4, 5, 9,      /* Jazz Preset */
                     -6, 4, 9, 4, -5,      /* Pop Preset */
                     10, 6, -1, 8, 10};   /* Rock Preset */

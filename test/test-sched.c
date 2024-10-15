#include "shared.h"
#include <sys/time.h>

// This is a multi-component tester that attempts to measure
// the multi-car scheduling performance of the controller
// This tester uses the following components:
// - call
// - car
// - controller

// You can control the simulation with the following arguments
// --car-delay (value)
// --cars (value)
// --num-passengers (value)
// --lowest-floor (floor name)
// --highest-floor (floor name)
// --sim-start (value)
// --sim-end (value)
// --histogram-len (number of bars on histogram)
// --svg (filename - produces an animated svg)

#define CAR_DELAY       "100" // string, milliseconds
#define CARS            1
#define NUM_PASSENGERS  10
#define LOWEST_FLOOR    "1"
#define HIGHEST_FLOOR   "4"
#define SIM_START       40    // milliseconds
#define SIM_END         1000  // milliseconds
#define HISTOGRAM_LEN   5

static double svg_timescale = 10.0;

// NUM_PASSENGERS will be scheduled to arrive at random
// times between SIM_START and SIM_END, and will arrive
// at a random floor with an intended destination of
// another random floor

typedef struct {
  char from[4], to[4], col[4];
  int delay;
  int idx;
  int64_t time_waiting;
  int64_t time_in_elevator;
} passenger_data;

typedef struct {
  int64_t minval;
  int64_t maxval;
  int count;
} histogram;

typedef struct {
  char name[16];
  struct timeval last_update;
  car_shared_mem mem;
  car_shared_mem *shm;
  pthread_cond_t cond;
  pthread_mutex_t mutex;
  pid_t pid;
  pthread_t tid;
  int cancel;
} car_tracker;

pid_t controller(void);
void car(car_tracker *, const char *, const char *, const char *, const char *);
int get_dir(int, int);
int fti(const char *);
void itf(char *, int);
int64_t us_diff(const struct timeval *, const struct timeval *);
void cleanup(pid_t);
void cleanup_tracker(car_tracker *);
void *sim_run(void *);

void svg_write(void);
void svg_add_event(struct timeval, int, int, int, int);

#define EV_STARTOPEN 1
#define EV_FINISHOPEN 2
#define EV_STARTCLOSE 3
#define EV_FINISHCLOSE 4
#define EV_NEWLIFT 5
#define EV_LIFTSTART 6
#define EV_LIFTFINISH 7
#define EV_WAITFORLIFT 8
#define EV_ENTERLIFT 9
#define EV_EXITLIFT 10

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define SVG_ANIM_ID "z"

static const char *car_delay = CAR_DELAY;
static int cars = CARS;
static int num_passengers = NUM_PASSENGERS;
static const char *lowest_floor = LOWEST_FLOOR;
static const char *highest_floor = HIGHEST_FLOOR;
static int sim_start = SIM_START;
static int sim_end = SIM_END;
static int histogram_len = HISTOGRAM_LEN;
static const char *svg = NULL;
static const char *svg_anim_id = SVG_ANIM_ID;

static car_tracker *car_trackers;
static passenger_data *pdata;
static struct timeval start_tv;

int rand_between(int min, int max) {
    int64_t v = rand();
    return v * (max - min + 1) / ((int64_t)RAND_MAX + 1) + min;
}

void draw_histogram(histogram *h) {
    int max_count = 0;
    for (int i = 0; i < histogram_len; i++) {
        max_count = MAX(max_count, h[i].count);
    }

    for (int i = 0; i < histogram_len; i++) {
        printf("%7.2f - %-7.2f ", (double)h[i].minval / 1000.0, (double)h[i].maxval / 1000.0);
        int len = h[i].count;
        if (max_count > 60) {
            len = (len * 60 + max_count - 1) / max_count;
        }
        for (int j = 0; j < len; j++) {
            printf("#");
        }
        printf(" (%d)\n", h[i].count);
    }
}

void init_args(int argc, char **argv)
{
    for (int i = 1; i < argc - 1; i+=2) {
        if (strcmp(argv[i], "--car-delay")==0) car_delay = argv[i+1];
        else if (strcmp(argv[i], "--cars")==0) cars = atoi(argv[i+1]);
        else if (strcmp(argv[i], "--num-passengers")==0) num_passengers = atoi(argv[i+1]);
        else if (strcmp(argv[i], "--lowest-floor")==0) lowest_floor = argv[i+1];
        else if (strcmp(argv[i], "--highest-floor")==0) highest_floor = argv[i+1];
        else if (strcmp(argv[i], "--sim-start")==0) sim_start = atoi(argv[i+1]);
        else if (strcmp(argv[i], "--sim-end")==0) sim_end = atoi(argv[i+1]);
        else if (strcmp(argv[i], "--histogram-len")==0) histogram_len = atoi(argv[i+1]);
        else if (strcmp(argv[i], "--svg")==0) svg = argv[i+1];
        else if (strcmp(argv[i], "--svg-anim-id")==0) svg_anim_id = argv[i+1];
        else if (strcmp(argv[i], "--svg-timescale")==0) svg_timescale = atof(argv[i+1]);
        else {
            fprintf(stderr, "Invalid parameter: %s\n", argv[i]);
            exit(1);
        }
    }
}

int main(int argc, char **argv)
{
    init_args(argc, argv);

    srand(time(NULL));
    gettimeofday(&start_tv, NULL);
    pid_t controller_pid = controller();
    car_trackers = malloc(sizeof(car_tracker) * cars);
    for (int i = 0; i < cars; i++) {
        char carname[16];
        sprintf(carname, "Sim%d", i + 1);
        car(&car_trackers[i], carname, lowest_floor, highest_floor, car_delay);
    }
    pthread_t passengers[num_passengers];
    pdata = malloc(sizeof(passenger_data) * num_passengers);
    for (int i = 0; i < num_passengers; i++) {
        itf(pdata[i].from, rand_between(fti(lowest_floor), fti(highest_floor)));
        for (;;) {
            itf(pdata[i].to, rand_between(fti(lowest_floor), fti(highest_floor)));
            if (strcmp(pdata[i].from, pdata[i].to) != 0) break;
        }
        pdata[i].delay = rand_between(sim_start * 1000, sim_end * 1000);
        int col = rand_between(0, 4095);
        sprintf(pdata[i].col, "%03x", col);
        col = rand_between(0, 2);
        pdata[i].col[col] = '0';
        pdata[i].idx = i;
        pthread_create(&passengers[i], NULL, sim_run, &pdata[i]);
    }

    int64_t total_wait_time = 0;
    int64_t total_spent_time = 0;
    int64_t min_wait_time = INT64_MAX;
    int64_t min_spent_time = INT64_MAX;
    int64_t max_wait_time = 0;
    int64_t max_spent_time = 0;
    for (int i = 0; i < num_passengers; i++) {
        pthread_join(passengers[i], NULL);

        total_wait_time += pdata[i].time_waiting;
        total_spent_time += pdata[i].time_in_elevator;
        max_wait_time = MAX(max_wait_time, pdata[i].time_waiting);
        max_spent_time = MAX(max_spent_time, pdata[i].time_in_elevator);
        min_wait_time = MIN(min_wait_time, pdata[i].time_waiting);
        min_spent_time = MIN(min_spent_time, pdata[i].time_in_elevator);
    }
    histogram_len = MIN(histogram_len, num_passengers);
    histogram histo_tw[histogram_len];
    histogram histo_ti[histogram_len];
    // Init histogram
    for (int i = 0; i < histogram_len; i++) {
        histo_tw[i].minval = min_wait_time + ((max_wait_time - min_wait_time + 1) * i / histogram_len);
        histo_tw[i].maxval = min_wait_time + ((max_wait_time - min_wait_time + 1) * (i + 1) / histogram_len - 1);
        histo_tw[i].count = 0;
        histo_ti[i].minval = min_spent_time + ((max_spent_time - min_spent_time + 1) * i / histogram_len);
        histo_ti[i].maxval = min_spent_time + ((max_spent_time - min_spent_time + 1) * (i + 1) / histogram_len - 1);
        histo_ti[i].count = 0;
    }
    
    for (int i = 0; i < num_passengers; i++) {
        for (int j = 0; j < histogram_len; j++) {
            if (pdata[i].time_waiting >= histo_tw[j].minval && pdata[i].time_waiting <= histo_tw[j].maxval) histo_tw[j].count++;
            if (pdata[i].time_in_elevator >= histo_ti[j].minval && pdata[i].time_in_elevator <= histo_ti[j].maxval) histo_ti[j].count++;
        }
    }
    printf("Time spent waiting for an elevator:\n");
    printf("Avg time: %.2fms\n", (double)total_wait_time / num_passengers / 1000.0);
    printf("Longest time: %.2fms\n", (double)max_wait_time / 1000.0);
    draw_histogram(histo_tw);
    printf("\n");
    printf("Time spent inside an elevator:\n");
    printf("Avg time: %.2fms\n", (double)total_spent_time / num_passengers / 1000.0);
    printf("Longest time: %.2fms\n", (double)max_spent_time / 1000.0);
    draw_histogram(histo_ti);

    for (int i = 0; i < cars; i++) {
        cleanup_tracker(&car_trackers[i]);
    }
    cleanup(controller_pid);

    svg_write();

    free(pdata);
    free(car_trackers);
    return 0;
}

void *sim_run(void *v)
{
    passenger_data *data = v;

    usleep(data->delay);
    char cmdbuf[256];
    sprintf(cmdbuf, "./call %s %s", data->from, data->to);
    FILE *fp = popen(cmdbuf, "r");
    char out[256];
    fread(out, 1, 256, fp);
    pclose(fp);
    char car[16];
    if (sscanf(out, "Car %s", car) < 1) {
        fprintf(stderr, "Invalid response from call: %s\n", out);
        return NULL;
    }
    int car_i = atoi(car+3) - 1;
    if (car_i == -1) {
        fprintf(stderr, "Invalid car name: %s\n", car);
        return NULL;
    }
    car_tracker *t = &car_trackers[car_i];

    // Wait for elevator to come
    struct timeval started_waiting;
    gettimeofday(&started_waiting, NULL);

    svg_add_event(started_waiting, EV_WAITFORLIFT, car_i, fti(data->from), data->idx);

    pthread_mutex_lock(&t->mutex);
    int passenger_dir = get_dir(fti(data->from), fti(data->to));
    for (;;) {
        // Elevator must be:
        // - On the passenger's floor
        // - Heading in the passenger's direction
        // - Open

        if (strcmp(t->mem.current_floor, data->from)==0) {
            if (strcmp(t->mem.status, "Open")==0) {
                int car_dir = get_dir(fti(t->mem.current_floor), fti(t->mem.destination_floor));
                if (car_dir == passenger_dir) {                    
                    // Get into elevator
                    break;
                }
            }
        }
        pthread_cond_wait(&t->cond, &t->mutex);
    }

    struct timeval elevator_arrived = t->last_update;
    svg_add_event(elevator_arrived, EV_ENTERLIFT, car_i, fti(data->from), data->idx);

    // Wait for elevator to arrive on destination floor
    for (;;) {
        if (strcmp(t->mem.current_floor, data->to)==0) {
            if (strcmp(t->mem.status, "Open")==0) {
                // Leave elevator
                break;
            }
        }
        pthread_cond_wait(&t->cond, &t->mutex);
    }
    pthread_mutex_unlock(&t->mutex);

    struct timeval passenger_arrived = t->last_update;
    svg_add_event(passenger_arrived, EV_EXITLIFT, car_i, fti(data->to), data->idx);
    data->time_waiting = us_diff(&started_waiting, &elevator_arrived);
    data->time_in_elevator = us_diff(&elevator_arrived, &passenger_arrived);

    return NULL;
}

int get_dir(int from, int to) {
  return (to - from) / abs(to - from);
}

int fti(const char *f)
{
    if (f[0] == 'B') return 1-atoi(f+1);
    else return atoi(f);
}

void itf(char *out, int f)
{
    if (f <= 0) sprintf(out, "B%d", 1-f);
    else sprintf(out, "%d", f);
}

int64_t us_diff(const struct timeval *before, const struct timeval *after)
{
    int64_t diff = ((int64_t)after->tv_sec - (int64_t)before->tv_sec) * (int64_t)1000000;
    diff += (int64_t)after->tv_usec - (int64_t)before->tv_usec;
    if (diff < 0) diff = 0;
    return diff;
}

void *car_tracking_thread(void *p) {
    car_tracker *t = p;
    pthread_mutex_lock(&t->mutex);
    
    usleep(SIM_START * 1000 / 2);
    char shmPath[24];
    sprintf(shmPath, "/car%s", t->name);

    // Attempt several times to map shared memory
    int shm_fd = -1;
    for (int at = 0; at < 10; at++) {
        shm_fd = shm_open(shmPath, O_RDWR, 0666);
        if (shm_fd != -1) break;
        usleep(SIM_START * 1000 / 20);
    }
    if (shm_fd == -1) {
        perror("shm_open");
        exit(1);
    }

    car_shared_mem *shm = mmap(0, sizeof(*shm), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    t->shm = shm;

    pthread_mutex_lock(&shm->mutex);

    int car_id = atoi(t->name + 3) - 1;

    int curr_floor = fti(shm->current_floor);
    svg_add_event(start_tv, EV_NEWLIFT, car_id, curr_floor, 0);

    int curr_open = 0;

    for (;;) {
        struct timeval curr_tv;
        gettimeofday(&curr_tv, NULL);
        car_shared_mem oldmem = t->mem;
        t->mem = *shm;
        car_shared_mem newmem = t->mem;
        t->last_update = curr_tv;
        pthread_mutex_unlock(&t->mutex);
        pthread_cond_broadcast(&t->cond);

        curr_floor = fti(newmem.current_floor);

        // Is this an animation event?
        if (svg) {
            if ((strcmp(oldmem.status, "Closed")==0 || strcmp(oldmem.status, "Between")==0) && strcmp(newmem.status, "Opening")==0) {
                svg_add_event(curr_tv, EV_STARTOPEN, car_id, curr_floor, 0);
            } else if (strcmp(oldmem.status, "Opening")==0 && strcmp(newmem.status, "Open")==0) {
                curr_open = 1;
                svg_add_event(curr_tv, EV_FINISHOPEN, car_id, curr_floor, 0);
            } else if (strcmp(oldmem.status, "Open")==0 && strcmp(newmem.status, "Closing")==0) {
                svg_add_event(curr_tv, EV_STARTCLOSE, car_id, curr_floor, 0);
            } else if (strcmp(oldmem.status, "Closing")==0 && (strcmp(newmem.status, "Closed")==0 || strcmp(newmem.status, "Between")==0)) {
                curr_open = 0;
                svg_add_event(curr_tv, EV_FINISHCLOSE, car_id, curr_floor, 0);
            }
            
            if (strcmp(oldmem.status, "Between")!=0 && strcmp(newmem.status, "Between")==0) {
                svg_add_event(curr_tv, EV_LIFTSTART, car_id, curr_floor, 0);
            } else if (strcmp(oldmem.status, "Between")==0 && strcmp(newmem.status, "Between")!=0) {
                svg_add_event(curr_tv, EV_LIFTFINISH, car_id, curr_floor, 0);
            }
        }
        if (curr_open == 0 && t->cancel == 1) {
            pthread_mutex_lock(&t->mutex);
            break;
        }

        pthread_cond_wait(&shm->cond, &shm->mutex);
        pthread_mutex_lock(&t->mutex);
        if (curr_open == 0 && t->cancel == 1) break;
    }
    pthread_mutex_unlock(&shm->mutex);
    pthread_mutex_unlock(&t->mutex);
    munmap(shm, sizeof(*shm));
    close(shm_fd);

    return NULL;
}
void car(car_tracker *t, const char *name, const char *lowest_floor, const char *highest_floor, const char *delay)
{
  pid_t pid = fork();
  if (pid == 0) {
    execlp("./car", "./car", name, lowest_floor, highest_floor, delay, NULL);
  }

  t->pid = pid;
  t->cancel = 0;
  pthread_mutex_init(&t->mutex, NULL);
  pthread_cond_init(&t->cond, NULL);
  strcpy(t->name, name);
  pthread_create(&t->tid, NULL, car_tracking_thread, t);
}
pid_t controller(void)
{
  pid_t pid = fork();
  if (pid == 0) {
    execlp("./controller", "./controller", NULL);
  }

  return pid;
}
void cleanup(pid_t p)
{
  kill(p, SIGINT);
}
void cleanup_tracker(car_tracker *t)
{
  pthread_mutex_lock(&t->mutex);
  t->cancel = 1;
  pthread_mutex_unlock(&t->mutex);
  pthread_cond_broadcast(&t->shm->cond);
  pthread_join(t->tid, NULL);
  kill(t->pid, SIGINT);
}

// SVG handling

static int svg_floorheight = 96;
static int svg_floorgap = 16;
static int svg_elevgap = 12;
static int svg_horigap = 48;
static int svg_elevw = 96;
static int svg_padw = 8;
static int svg_padh = 16;

static int svg_personsize = 12;

static int svg_bottommargin = 16;
static int svg_progressbar_w = 32;
static int svg_progressbar_h = 16;

double svgtime(const struct timeval *before, const struct timeval *after) {
    return us_diff(before, after) / 1000000.0 * svg_timescale;
}

typedef struct svg_event {
    struct timeval tv;
    int type, x, y, z;
    struct svg_event *next;
} svg_event;

static svg_event *svg_head = NULL;
static svg_event *svg_tail = NULL;
static pthread_mutex_t svg_mutex = PTHREAD_MUTEX_INITIALIZER;

void svg_add_event(struct timeval tv, int type, int x, int y, int z) {
    if (!svg) return;
    svg_event *e = malloc(sizeof(svg_event));
    e->tv = tv;
    e->type = type;
    e->x = x;
    e->y = y;
    e->z = z;
    e->next = NULL;
    
    pthread_mutex_lock(&svg_mutex);
    if (!svg_tail) {
        svg_head = svg_tail = e;
    } else {
        svg_tail->next = e;
        svg_tail = e;
    }
    pthread_mutex_unlock(&svg_mutex);
}

int svg_compar(const void *va, const void *vb) {
    struct timeval a = ((const svg_event *)va)->tv;
    struct timeval b = ((const svg_event *)vb)->tv;
    if (a.tv_sec < b.tv_sec) return -1;
    if (a.tv_sec > b.tv_sec) return 1;
    if (a.tv_usec < b.tv_usec) return -1;
    if (a.tv_usec > b.tv_usec) return 1;
    return 0;
}

void svg_draw_elevator(FILE *fp, svg_event *ev, int count, int car_id);
void svg_draw_passenger(FILE *fp, svg_event *ev, int count, int pass_id, int fg);
void svg_draw_doors(FILE *fp, svg_event *ev, int count, int car_id, int floor);
void svg_draw_background(FILE *fp);

static int svg_width, svg_height;

void svg_write(void) {
    if (!svg) return;
    
    // Pull events into a single array and sort them
    svg_event *ev;
    int count = 0;

    {
        svg_event *t = svg_head;
        while (t != NULL) {
            count++;
            t = t->next;
        }
        ev = malloc(sizeof(svg_event) * count);
        int idx = 0;
        t = svg_head;
        while (t != NULL) {
            ev[idx] = *t;
            free(t);
            t = ev[idx].next;
            idx++;
        }
        qsort(ev, count, sizeof(*ev), svg_compar);
    }

    svg_width = svg_horigap + svg_padw + cars * (svg_horigap + svg_elevw) + svg_horigap + svg_padw + svg_horigap;
    int floors = fti(highest_floor) - fti(lowest_floor) + 1;
    svg_height = (svg_floorheight + svg_floorgap) * floors - svg_floorgap;
    svg_height += svg_bottommargin;
    int footer_start = svg_height;
    svg_height += svg_progressbar_h;
    FILE *fp = fopen(svg, "w");
    fprintf(fp, "<svg version=\"1.1\" width=\"%d\" height=\"%d\" xmlns=\"http://www.w3.org/2000/svg\">", svg_width, svg_height);
    fprintf(fp, "<defs><g id=\"p\"><circle r=\"%d\" cx=\"%d\" cy=\"%d\" /><rect x=\"0\" y=\"%d\" width=\"%d\" height=\"%d\" /></g></defs>",
        svg_personsize, svg_personsize, svg_personsize, svg_personsize * 2, svg_personsize * 2, svg_personsize * 2
    );
    // Layers, from back to front
    // - Elevator cars
    // - Passengers inside cars
    // - Building background (partially transparent fill, so cars/passengers can be seen between levels)
    // - Passengers outside cars

    for (int i = 0; i < cars; i++) {
        svg_draw_elevator(fp, ev, count, i);
    }

    for (int i = 0; i < num_passengers; i++) {
        svg_draw_passenger(fp, ev, count, i, 0);
    }

    for (int i = 0; i < cars; i++) {
        for (int j = fti(lowest_floor); j <= fti(highest_floor); j++) {
            svg_draw_doors(fp, ev, count, i, j);
        }
    }

    svg_draw_background(fp);

    for (int i = 0; i < num_passengers; i++) {
        svg_draw_passenger(fp, ev, count, i, 1);
    }

    // Progress bar

    fprintf(fp, "<path d=\"M%d %d L%d %d M%d %d L%d %d M%d %d L%d %d\" style=\"stroke: black;\" />\n",
        0, footer_start + svg_progressbar_h/2,
        svg_width, footer_start + svg_progressbar_h/2,
        0, footer_start,
        0, footer_start + svg_progressbar_h,
        svg_width, footer_start,
        svg_width, footer_start + svg_progressbar_h
    );
    fprintf(fp, "<rect width=\"%d\" height=\"%d\" x=\"0\" y=\"%d\">\n", svg_progressbar_w, svg_progressbar_h, footer_start);
    double total_dur = svgtime(&start_tv, &ev[count-1].tv);
    fprintf(fp, "  <animate id=\"%s\" attributeName=\"x\" begin=\"0s;%s.end\" dur=\"%.2fs\" from=\"0\" to=\"%d\" />",
        svg_anim_id, svg_anim_id, total_dur, svg_width - svg_progressbar_w);
    fprintf(fp, "</rect>\n");

    fprintf(fp, "</svg>\n");
    fclose(fp);

    free(ev);
    pthread_mutex_unlock(&svg_mutex);
}

void svg_draw_elevator(FILE *fp, svg_event *ev, int count, int car_id) {
    int x = svg_horigap + svg_padw + car_id * (svg_horigap + svg_elevw) + svg_horigap;
    int curr_floor;
    struct timeval lift_start_tv, lift_finish_tv;

    for (int i = 0; i < count; i++) {
        if (ev[i].x != car_id) continue;
        if (ev[i].type == EV_NEWLIFT) {
            curr_floor = ev[i].y;
            int curr_floor_pos = fti(highest_floor) - curr_floor;
            int y = (svg_floorheight + svg_floorgap) * curr_floor_pos + svg_elevgap;
            int elevh = svg_floorheight - svg_elevgap * 2;
            fprintf(fp, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" style=\"fill: none; stroke: black; stroke-width: 5;\">\n", x, y, svg_elevw, elevh);
            fprintf(fp, "  <set attributeName=\"y\" to=\"%d\" begin=\"%s.begin\" />\n", y, svg_anim_id);
        } else if (ev[i].type == EV_LIFTSTART) {
            lift_start_tv = ev[i].tv;
        } else if (ev[i].type == EV_LIFTFINISH) {
            lift_finish_tv = ev[i].tv;
            int dest_floor = ev[i].y;
            int curr_floor_pos = fti(highest_floor) - curr_floor;
            int dest_floor_pos = fti(highest_floor) - dest_floor;
            int sy = (svg_floorheight + svg_floorgap) * curr_floor_pos + svg_elevgap;
            int dy = (svg_floorheight + svg_floorgap) * dest_floor_pos + svg_elevgap;
            double begin = svgtime(&start_tv, &lift_start_tv);
            double dur = svgtime(&lift_start_tv, &lift_finish_tv);
            fprintf(fp, "  <animate attributeName=\"y\" values=\"%d;%d\" begin=\"%s.begin+%.2fs\" dur=\"%.2fs\" fill=\"freeze\"/>\n", sy, dy, svg_anim_id, begin, dur);
            curr_floor = dest_floor;
        }
    }
    fprintf(fp, "</rect>\n");
}

#define PASSENGER_BASE_SPEED 1000
double svg_passenger_dist_to_secs(double dist) {
    double pixels_per_second = PASSENGER_BASE_SPEED / svg_timescale;
    return dist / pixels_per_second;
}
double svg_passenger_secs_to_dist(double secs) {
    double pixels_per_second = PASSENGER_BASE_SPEED / svg_timescale;
    return secs * pixels_per_second;
}

void svg_draw_passenger(FILE *fp, svg_event *ev, int count, int pass_id, int fg) {
    const char *col = pdata[pass_id].col;
    int from_right = pass_id % 2;
    double adjusted_delay = atoi(CAR_DELAY) * 0.001 * svg_timescale;

    double called_lift_at;
    double lift_opened;
    double exited_lift;
    int srcfloor, destfloor;
    int car_id;
    int enter_lift_i = -1;
    for (int i = 0; i < count; i++) {
        if (ev[i].z != pass_id) continue;
        if (ev[i].type == EV_WAITFORLIFT) {
            car_id = ev[i].x;
            called_lift_at = svgtime(&start_tv, &ev[i].tv);
        }
        else if (ev[i].type == EV_ENTERLIFT) {
            enter_lift_i = i;
            srcfloor = ev[i].y;
            lift_opened = svgtime(&start_tv, &ev[i].tv);
        }
        else if (ev[i].type == EV_EXITLIFT) {
            destfloor = ev[i].y;
            exited_lift = svgtime(&start_tv, &ev[i].tv);
        }
    }

    if (called_lift_at + adjusted_delay >= lift_opened) {
        called_lift_at = lift_opened - adjusted_delay;
    }

    // Walking up to callpad
    int srcfloorpos = fti(highest_floor) - srcfloor;
    int leadup = svg_passenger_secs_to_dist(called_lift_at);

    int x;
    if (!from_right) {
        x = svg_horigap + svg_padw / 2 - svg_personsize;
    } else {
        x = svg_width - svg_horigap - svg_padw / 2 - svg_personsize;
        leadup *= -1;
    }
    int y = (svg_floorheight + svg_floorgap) * srcfloorpos + svg_elevgap + svg_floorheight/2 - svg_personsize*2;
    
    if (fg) {
        fprintf(fp, "<use href=\"#p\" fill=\"#%s\">\n", col);
    } else {
        fprintf(fp, "<use href=\"#p\" fill=\"#%s\" x=\"%d\">\n", col, svg_personsize*-2);
    }
    fprintf(fp, "  <set attributeName=\"y\" to=\"%d\" begin=\"%s.begin\" />\n", y, svg_anim_id);
    // Just the foreground one needs to appear here
    if (fg) {
        fprintf(fp, "  <animate attributeName=\"x\" values=\"%d;%d\" begin=\"%s.begin\" dur=\"%.2fs\" fill=\"freeze\" />\n", x - leadup, x, svg_anim_id, called_lift_at);
    }

    // Walking up to elevator we're waiting at.

    // Locate elevator
    int wait_x = svg_horigap + svg_padw + car_id * (svg_horigap + svg_elevw) + svg_horigap + svg_personsize;

    // Random location inside elevator
    {
        // Knuth LCG
        int r = (pass_id * 1664525 + 1013904223) % (svg_elevw - svg_personsize*4);
        wait_x += r;
    }

    // Brief pause at call pad - makes this look more natural
    double pause_at_callpad = atof(car_delay) * 0.001 * svg_timescale / 4;

    // Calculate normal walking time
    double standard_walking_time = svg_passenger_dist_to_secs(fabs(wait_x - x));

    // However, passenger must arrive by the time the lift opens
    double wait_ts = MIN(called_lift_at + pause_at_callpad + standard_walking_time, lift_opened);
    if (fg) {
        fprintf(fp, "  <animate attributeName=\"x\" values=\"%d;%d\" begin=\"%s.begin+%.2fs\" dur=\"%.2fs\" fill=\"freeze\" />\n", x, wait_x, svg_anim_id, called_lift_at + pause_at_callpad, wait_ts - called_lift_at - pause_at_callpad);
    }

    // Entering elevator once the doors open. For this the fg graphic will fade out and the bg graphic will appear

    if (fg) {
        fprintf(fp, "  <animate attributeName=\"opacity\" values=\"1;0\" begin=\"%s.begin+%.2fs\" dur=\"%.2fs\" fill=\"freeze\" />\n", svg_anim_id, lift_opened, adjusted_delay / 2);
    } else {
        fprintf(fp, "  <set attributeName=\"x\" to=\"%d\" begin=\"%s.begin+%.2fs\" />\n", wait_x, svg_anim_id, lift_opened);
    }

    double elev_start, elev_finish = -999.0;
    // Find when this elevator next moves and go with it
    int curr_floor = srcfloor;
    int elev_y = y;
    for (int i = enter_lift_i + 1; i < count; i++) {
        if (ev[i].type != EV_LIFTSTART) continue;
        if (ev[i].x != car_id) continue;

        elev_start = svgtime(&start_tv, &ev[i].tv);
        if (elev_start < lift_opened) continue;
        for (int j = i + 1; j < count; j++) {
            if (ev[j].type != EV_LIFTFINISH) continue;
            if (ev[j].x != car_id) continue;
            
            elev_finish = svgtime(&start_tv, &ev[j].tv);
            curr_floor = ev[j].y;
            
            int frompos = fti(highest_floor) - ev[i].y;
            int topos = fti(highest_floor) - ev[j].y;
            int y_offset = (topos - frompos);

            int new_y = elev_y + y_offset * (svg_floorheight + svg_floorgap);

            if (!fg) {
                fprintf(fp, "  <animate attributeName=\"y\" values=\"%d;%d\" begin=\"%s.begin+%.2fs\" dur=\"%.2fs\" fill=\"freeze\" />\n", elev_y, new_y, svg_anim_id, elev_start, elev_finish - elev_start);
            }
            elev_y = new_y;

            break;
        }
        if (curr_floor == destfloor) break;
    }

    // Exit elevator
    // Foreground will fade back in
    if (fg) {
        fprintf(fp, "  <set attributeName=\"y\" to=\"%d\" begin=\"%s.begin+%.2fs\" />\n", elev_y, svg_anim_id, exited_lift);
        fprintf(fp, "  <animate attributeName=\"opacity\" values=\"0;1\" begin=\"%s.begin+%.2fs\" dur=\"%.2fs\" fill=\"freeze\" />\n", svg_anim_id, exited_lift, adjusted_delay / 2);
    }
    // Leave

    int leave_x;
    if ((pass_id / 2) % 2 == 0) {
        // Leave left
        leave_x = svg_personsize * -2;
    } else {
        // Leave right
        leave_x = svg_width;
    }
    double leave_time = svg_passenger_dist_to_secs(fabs(leave_x - wait_x));
    if (fg) {
        fprintf(fp, "  <animate attributeName=\"x\" values=\"%d;%d\" begin=\"%s.begin+%.2fs\" dur=\"%.2fs\" fill=\"freeze\" />\n", wait_x, leave_x, svg_anim_id, exited_lift + adjusted_delay / 2, leave_time);
    } else {
        fprintf(fp, "  <set attributeName=\"x\" to=\"%d\" begin=\"%s.begin+%.2fs\" />\n", svg_personsize*-2, svg_anim_id, exited_lift + adjusted_delay / 2);
    }
    
    fprintf(fp, "</use>\n");
}

void svg_draw_doors(FILE *fp, svg_event *ev, int count, int car_id, int floor) {
    const char *style = "fill: #ccc; fill-opacity: 0.8; stroke: black";
    int floor_pos = fti(highest_floor) - floor;

    int x = svg_horigap + svg_padw + car_id * (svg_horigap + svg_elevw) + svg_horigap;
    int y = (svg_floorheight + svg_floorgap) * floor_pos + svg_elevgap;
    int elevh = svg_floorheight - svg_elevgap * 2;

    for (int door = 0; door < 2; door++) {
        int xpos = x + door * svg_elevw / 2;
        int openmove = door == 0 ? svg_elevw / -2 : svg_elevw / 2;
        fprintf(fp, "  <rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" style=\"%s\">\n", xpos, y, svg_elevw / 2, elevh, style);
        fprintf(fp, "    <set attributeName=\"x\" to=\"%d\" begin=\"%s.begin\" />\n", xpos, svg_anim_id);
        
        // Add animations
        for (int i = 0; i < count; i++) {
            if (ev[i].x != car_id) continue;
            if (fti(highest_floor) - ev[i].y != floor_pos) continue;
            if (ev[i].type != EV_STARTOPEN && ev[i].type != EV_STARTCLOSE) continue;                  

            for (int j = i + 1; j < count; j++) {
                if (ev[j].x != ev[i].x || ev[j].y != ev[i].y) continue;
                double begin = svgtime(&start_tv, &ev[i].tv);
                double dur = svgtime(&ev[i].tv, &ev[j].tv);

                if (ev[i].type == EV_STARTOPEN && ev[j].type == EV_FINISHOPEN) {
                    fprintf(fp, "    <animate attributeName=\"x\" begin=\"%s.begin+%.2fs\" dur=\"%.2fs\" values=\"%d;%d\" fill=\"freeze\"/>\n",
                        svg_anim_id, begin, dur, xpos, xpos + openmove
                    );
                    break;
                } else if (ev[i].type == EV_STARTCLOSE && ev[j].type == EV_FINISHCLOSE) {
                    fprintf(fp, "    <animate attributeName=\"x\" begin=\"%s.begin+%.2fs\" dur=\"%.2fs\" values=\"%d;%d\" fill=\"freeze\"/>\n",
                        svg_anim_id, begin, dur, xpos + openmove, xpos
                    );
                    break;
                }
            }
        }
        fprintf(fp, "  </rect>\n");
    }
}

void svg_draw_background(FILE *fp) {
    int elevh = svg_floorheight - svg_elevgap * 2;
    int floors = fti(highest_floor) - fti(lowest_floor) + 1;
    fprintf(fp, "<path d=\"");
    for (int floor_pos = 0; floor_pos < floors; floor_pos++) {
        
        int y = floor_pos * (svg_floorheight + svg_floorgap);
        
        // Clockwise
        fprintf(fp, "M%d %d ", 0, y);
        fprintf(fp, "L%d %d ", svg_width, y);
        fprintf(fp, "L%d %d ", svg_width, y + svg_floorheight);
        fprintf(fp, "L%d %d ", 0, y + svg_floorheight);
        fprintf(fp, "L%d %d ", 0, y);

        for (int car_id = 0; car_id < cars; car_id++) {
            int x = svg_horigap + svg_padw + svg_horigap + (svg_elevw + svg_horigap) * car_id;

            // Anti-clockwise
            fprintf(fp, "M%d %d ", x, y + svg_elevgap);
            fprintf(fp, "L%d %d ", x, y + svg_elevgap + elevh);
            fprintf(fp, "L%d %d ", x + svg_elevw, y + svg_elevgap + elevh);
            fprintf(fp, "L%d %d ", x + svg_elevw, y + svg_elevgap);
            fprintf(fp, "L%d %d ", x, y + svg_elevgap);
        }
        

    }
    fprintf(fp, "\" style=\"stroke: black; fill: #ddd; fill-opacity: 0.8\" />\n");

    for (int floor_pos = 0; floor_pos < floors; floor_pos++) {
        int floor = fti(highest_floor) - floor_pos;
        char floor_name[4];
        itf(floor_name, floor);

        int y = floor_pos * (svg_floorheight + svg_floorgap);

        fprintf(fp, "<text x=\"%d\" y=\"%d\" style=\"font-size: 24px; font-weight: bold; font-family: sans-serif; text-anchor: middle\">Floor %s</text>\n", svg_horigap + svg_padw / 2, y + svg_floorheight / 2 - 24, floor_name);
        fprintf(fp, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" style=\"fill: none; stroke: black;\" />\n", svg_horigap, y + svg_floorheight / 2, svg_padw, svg_padh);
        fprintf(fp, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" style=\"fill: none; stroke: black;\" />\n", svg_width - svg_horigap - svg_padw, y + svg_floorheight / 2, svg_padw, svg_padh);
        if (floor_pos < floors - 1) {
            fprintf(fp, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" style=\"fill: #aaa; fill-opacity: 0.9;\" />\n", 0, y + svg_floorheight, svg_width, svg_floorgap);
        }
    }
}

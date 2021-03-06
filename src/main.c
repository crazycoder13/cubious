#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "db.h"
#include "map.h"
#include "noise.h"
#include "util.h"
#include "client.h"

#define VSYNC 1
#define FULLSCREEN 0
#define SHOW_FPS 1
#define CHUNK_SIZE 32
#define MAX_CHUNKS 1024
#define CREATE_CHUNK_RADIUS 6
#define RENDER_CHUNK_RADIUS 6
#define DELETE_CHUNK_RADIUS 8
#define TEXT_BUFFER_SIZE 256

static GLFWwindow *window;
static int exclusive = 1;
static int left_click = 0;
static int right_click = 0;
static int flying = 0;
static int block_type = 1;
static int ortho = 0;
static int typing = 0;
char message[TEXT_BUFFER_SIZE] = {0};
char typing_buffer[TEXT_BUFFER_SIZE] = {0};

typedef struct {
    Map map;
    int p;
    int q;
    int faces;
    GLuint position_buffer;
    GLuint normal_buffer;
    GLuint uv_buffer;
} Chunk;

int is_plant(int w) {
	return w > 16 && w != 32;
}

int is_obstacle(int w) {
	return w != 0 && w <= 8;
}

int is_transparent(int w) {
	return w == 0 || w == 4 || w == 7 || is_plant(w);;
}

void update_matrix_2d(float *matrix) {
    int width, height;
    glfwGetWindowSize(window, &width, &height);
    glViewport(0, 0, width, height);
    mat_ortho(matrix, 0, width, 0, height, -1, 1);
}

void update_matrix_3d(
    float *matrix, float x, float y, float z, float rx, float ry)
{
    float a[16];
    float b[16];
    int width, height;
    glfwGetWindowSize(window, &width, &height);
    glViewport(0, 0, width, height);
    float aspect = (float)width / height;
    mat_identity(a);
    mat_translate(b, -x, -y, -z);
    mat_multiply(a, b, a);
    mat_rotate(b, cosf(rx), 0, sinf(rx), ry);
    mat_multiply(a, b, a);
    mat_rotate(b, 0, 1, 0, -rx);
    mat_multiply(a, b, a);
    if (ortho) {
        int size = 32;
        mat_ortho(b, -size * aspect, size * aspect, -size, size, -256, 256);
    }
    else {
        mat_perspective(b, 65.0, aspect, 0.1, 1024.0);
    }
    mat_multiply(a, b, a);
    for (int i = 0; i < 16; i++) {
        matrix[i] = a[i];
    }
}

GLuint make_line_buffer() {
    int width, height;
    glfwGetWindowSize(window, &width, &height);
    int x = width / 2;
    int y = height / 2;
    int p = 10;
    float data[] = {
        x, y - p, x, y + p,
        x - p, y, x + p, y
    };
    GLuint buffer = make_buffer(
        GL_ARRAY_BUFFER, sizeof(data), data
    );
    return buffer;
}

GLuint make_cube_buffer(float x, float y, float z, float n) {
    float data[144];
    make_cube_wireframe(data, x, y, z, n);
    GLuint buffer = make_buffer(
        GL_ARRAY_BUFFER, sizeof(data), data
    );
    return buffer;
}

void get_sight_vector(float rx, float ry, float *vx, float *vy, float *vz) {
    float m = cosf(ry);
    *vx = cosf(rx - RADIANS(90)) * m;
    *vy = sinf(ry);
    *vz = sinf(rx - RADIANS(90)) * m;
}

void get_motion_vector(int flying, int sz, int sx, float rx, float ry,
    float *vx, float *vy, float *vz) {
    *vx = 0; *vy = 0; *vz = 0;
    if (!sz && !sx) {
        return;
    }
    float strafe = atan2f(sz, sx);
    if (flying) {
        float m = cosf(ry);
        float y = sinf(ry);
        if (sx) {
            y = 0;
            m = 1;
        }
        if (sz > 0) {
            y = -y;
        }
        *vx = cosf(rx + strafe) * m;
        *vy = y;
        *vz = sinf(rx + strafe) * m;
    }
    else {
        *vx = cosf(rx + strafe);
        *vy = 0;
        *vz = sinf(rx + strafe);
    }
}

Chunk *find_chunk(Chunk *chunks, int chunk_count, int p, int q) {
    for (int i = 0; i < chunk_count; i++) {
        Chunk *chunk = chunks + i;
        if (chunk->p == p && chunk->q == q) {
            return chunk;
        }
    }
    return 0;
}

int chunk_distance(Chunk *chunk, int p, int q) {
    int dp = ABS(chunk->p - p);
    int dq = ABS(chunk->q - q);
    return MAX(dp, dq);
}

// Text buffer - Render textured text - Backported from craft
void gen_text_buffers(
	GLuint *position_buffer, GLuint *uv_buffer,
	float x, float y, float n, float m, char *text)
{
	int length = strlen(text);
	GLfloat *position_data, *uv_data;
	malloc_buffers(2, length, &position_data, 0, &uv_data);
	for (int i = 0; i < length; i++) {
		make_character(
			position_data + i * 12,
			uv_data + i * 12,
			x, y, n, m, text[i]);
		x += n * 2;
	}
	gen_buffers(
		2, length, position_data, 0, uv_data,
		position_buffer, 0, uv_buffer);
}

void draw_text(
	GLuint position_buffer, GLuint uv_buffer,
	GLuint position_loc, GLuint uv_loc, int length)
{
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnableVertexAttribArray(position_loc);
	glEnableVertexAttribArray(uv_loc);
	glBindBuffer(GL_ARRAY_BUFFER, position_buffer);
	glVertexAttribPointer(position_loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glBindBuffer(GL_ARRAY_BUFFER, uv_buffer);
	glVertexAttribPointer(uv_loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDrawArrays(GL_TRIANGLES, 0, length * 6);
	glDisableVertexAttribArray(position_loc);
	glDisableVertexAttribArray(uv_loc);
	glDisable(GL_BLEND);
}

void print(
	GLuint position_loc, GLuint uv_loc,
	float x, float y, float n, char *text)
{
	GLuint position_buffer = 0;
	GLuint uv_buffer = 0;
	gen_text_buffers(
		&position_buffer, &uv_buffer,
		x, y, n, n * 2, text);
	draw_text(
		position_buffer, uv_buffer,
		position_loc, uv_loc, strlen(text));
	glDeleteBuffers(1, &position_buffer);
	glDeleteBuffers(1, &uv_buffer);
}

int chunk_visible(Chunk *chunk, float *matrix) {
    for (int dp = 0; dp <= 1; dp++) {
        for (int dq = 0; dq <= 1; dq++) {
            for (int y = 0; y < 128; y += 16) {
                float vec[4] = {
                    (chunk->p + dp) * CHUNK_SIZE - dp,
                    y,
                    (chunk->q + dq) * CHUNK_SIZE - dq,
                    1};
                mat_vec_multiply(vec, matrix, vec);
                if (vec[3] >= 0) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

int highest_block(Chunk *chunks, int chunk_count, float x, float z) {
    int result = -1;
    int nx = roundf(x);
    int nz = roundf(z);
    int p = floorf(roundf(x) / CHUNK_SIZE);
    int q = floorf(roundf(z) / CHUNK_SIZE);
    Chunk *chunk = find_chunk(chunks, chunk_count, p, q);
    if (chunk) {
        Map *map = &chunk->map;
        MAP_FOR_EACH(map, e) {
            if (is_obstacle(e->w) && e->x == nx && e->z == nz) {
                result = MAX(result, e->y);
            }
        } END_MAP_FOR_EACH;
    }
    return result;
}

int _hit_test(
    Map *map, float max_distance, int previous,
    float x, float y, float z,
    float vx, float vy, float vz,
    int *hx, int *hy, int *hz)
{
    int m = 8;
    int px = 0;
    int py = 0;
    int pz = 0;
    for (int i = 0; i < max_distance * m; i++) {
        int nx = roundf(x);
        int ny = roundf(y);
        int nz = roundf(z);
        if (nx != px || ny != py || nz != pz) {
            //if (map_get(map, nx, ny, nz)) {
            int hw = map_get(map, nx, ny, nz);
            if (hw > 0) {
                if (previous) {
                    *hx = px; *hy = py; *hz = pz;
                }
                else {
                    *hx = nx; *hy = ny; *hz = nz;
                }
                return hw;
            }
            px = nx; py = ny; pz = nz;
        }
        x += vx / m; y += vy / m; z += vz / m;
    }
    return 0;
}

int hit_test(
    Chunk *chunks, int chunk_count, int previous,
    float x, float y, float z, float rx, float ry,
    int *bx, int *by, int *bz)
{
    int result = 0;
    float best = 0;
    int p = floorf(roundf(x) / CHUNK_SIZE);
    int q = floorf(roundf(z) / CHUNK_SIZE);
    float vx, vy, vz;
    get_sight_vector(rx, ry, &vx, &vy, &vz);
    for (int i = 0; i < chunk_count; i++) {
        Chunk *chunk = chunks + i;
        if (chunk_distance(chunk, p, q) > 1) {
            continue;
        }
        int hx, hy, hz;
        int hw = _hit_test(&chunk->map, 8, previous,
            x, y, z, vx, vy, vz, &hx, &hy, &hz);
        if (hw > 0)
        {
            float d = sqrtf(
                powf(hx - x, 2) + powf(hy - y, 2) + powf(hz - z, 2));
            if (best == 0 || d < best) {
                best = d;
                *bx = hx; *by = hy; *bz = hz;
            }
            result = 1;
        }
    }
    return result;
}

int collide(
    Chunk *chunks, int chunk_count,
    int height, float *x, float *y, float *z)
{
    int result = 0;
    int p = floorf(roundf(*x) / CHUNK_SIZE);
    int q = floorf(roundf(*z) / CHUNK_SIZE);
    Chunk *chunk = find_chunk(chunks, chunk_count, p, q);
    if (!chunk) {
        return result;
    }
    Map *map = &chunk->map;
    int nx = roundf(*x);
    int ny = roundf(*y);
    int nz = roundf(*z);
    float px = *x - nx;
    float py = *y - ny;
    float pz = *z - nz;
    float pad = 0.25;
    for (int dy = 0; dy < height; dy++) {
        if (px < -pad && is_obstacle(map_get(map, nx - 1, ny - dy, nz))) {
            *x = nx - pad;
        }
        if (px > pad && is_obstacle(map_get(map, nx + 1, ny - dy, nz))) {
            *x = nx + pad;
        }
        if (py < -pad && is_obstacle(map_get(map, nx, ny - dy - 1, nz))) {
            *y = ny - pad;
            result = 1;
        }
        if (py > pad && is_obstacle(map_get(map, nx, ny - dy + 1, nz))) {
            *y = ny + pad;
            result = 1;
        }
        if (pz < -pad && is_obstacle(map_get(map, nx, ny - dy, nz - 1))) {
            *z = nz - pad;
        }
        if (pz > pad && is_obstacle(map_get(map, nx, ny - dy, nz + 1))) {
            *z = nz + pad;
        }
    }
    return result;
}

int player_intersects_block(
    int height,
    float x, float y, float z,
    int hx, int hy, int hz)
{
    int nx = roundf(x);
    int ny = roundf(y);
    int nz = roundf(z);
    for (int i = 0; i < height; i++) {
        if (nx == hx && ny - i == hy && nz == hz) {
            return 1;
        }
    }
    return 0;
}

void exposed_faces(
    Map *map, int x, int y, int z,
    int *f1, int *f2, int *f3, int *f4, int *f5, int *f6)
{
	
	 
    *f1 = is_transparent(map_get(map, x - 1, y, z));
    *f2 = is_transparent(map_get(map, x + 1, y, z));
    *f3 = is_transparent(map_get(map, x, y + 1, z));
    *f4 = is_transparent(map_get(map, x, y - 1, z)) && (y > 0);
    *f5 = is_transparent(map_get(map, x, y, z + 1));
    *f6 = is_transparent(map_get(map, x, y, z - 1));
    
    /*
    *f1 = map_get(map, x - 1, y, z) == 0;
    *f2 = map_get(map, x + 1, y, z) == 0;
    *f3 = map_get(map, x, y + 1, z) == 0;
    *f4 = map_get(map, x, y - 1, z) == 0 && y > 0;
    *f5 = map_get(map, x, y, z + 1) == 0;
    *f6 = map_get(map, x, y, z - 1) == 0;
    */
}

void update_chunk(Chunk *chunk) {
    Map *map = &chunk->map;

    if (chunk->faces) {
        glDeleteBuffers(1, &chunk->position_buffer);
        glDeleteBuffers(1, &chunk->normal_buffer);
        glDeleteBuffers(1, &chunk->uv_buffer);
    }

    int faces = 0;
    MAP_FOR_EACH(map, e) {
        if (e->w <= 0) {
            continue;
        }
        int f1, f2, f3, f4, f5, f6;
        exposed_faces(map, e->x, e->y, e->z, &f1, &f2, &f3, &f4, &f5, &f6);
        int total = f1 + f2 + f3 + f4 + f5 + f6;
        if(is_plant(e->w)) {
			total = total ? 4 : 0;
		}
        faces += total;
    } END_MAP_FOR_EACH;

    GLfloat *position_data = malloc(sizeof(GLfloat) * faces * 18);
    GLfloat *normal_data = malloc(sizeof(GLfloat) * faces * 18);
    GLfloat *uv_data = malloc(sizeof(GLfloat) * faces * 12);
    int position_offset = 0;
    int uv_offset = 0;
    MAP_FOR_EACH(map, e) {
        if (e->w <= 0) {
            continue;
        }
        int f1, f2, f3, f4, f5, f6;
        exposed_faces(map, e->x, e->y, e->z, &f1, &f2, &f3, &f4, &f5, &f6);
        int total = f1 + f2 + f3 + f4 + f5 + f6;
        
        if(is_plant(e->w)) {
			total = total ? 4 : 0;
		}
		
        if(total == 0) {
            continue;
        }
        
        if(is_plant(e->w)) {
			float rotation = simplex3(e->x, e->y, e->z, 4, 0.5, 2) * 360;
			make_plant(
				position_data + position_offset,
				normal_data + position_offset,
				uv_data + uv_offset,
				e->x, e->y, e->z, 0.5, e->w, rotation);
		} else {
			make_cube(
				position_data + position_offset,
				normal_data + position_offset,
				uv_data + uv_offset,
				f1, f2, f3, f4, f5, f6,
				e->x, e->y, e->z, 0.5, e->w);
		}
        position_offset += total * 18;
        uv_offset += total * 12;
    } END_MAP_FOR_EACH;

    GLuint position_buffer = make_buffer(
        GL_ARRAY_BUFFER,
        sizeof(GLfloat) * faces * 18,
        position_data
    );
    GLuint normal_buffer = make_buffer(
        GL_ARRAY_BUFFER,
        sizeof(GLfloat) * faces * 18,
        normal_data
    );
    GLuint uv_buffer = make_buffer(
        GL_ARRAY_BUFFER,
        sizeof(GLfloat) * faces * 12,
        uv_data
    );
    free(position_data);
    free(normal_data);
    free(uv_data);

    chunk->faces = faces;
    chunk->position_buffer = position_buffer;
    chunk->normal_buffer = normal_buffer;
    chunk->uv_buffer = uv_buffer;
}

void make_chunk(Chunk *chunk, int p, int q) {
	char buffer[1024];
	printf("Client -> Server: Requesting chunk creation [%d, %d]\n", p, q);
	snprintf(buffer, 1024, "C,%d,%d\n", p, q);
	client_send(buffer);
    chunk->p = p;
    chunk->q = q;
    chunk->faces = 0;
    Map *map = &chunk->map;
    map_alloc(map);
    make_world(map, p, q);
    update_chunk(chunk);
}

void draw_chunk(
    Chunk *chunk, GLuint position_loc, GLuint normal_loc, GLuint uv_loc)
{
    glEnableVertexAttribArray(position_loc);
    glEnableVertexAttribArray(normal_loc);
    glEnableVertexAttribArray(uv_loc);
    glBindBuffer(GL_ARRAY_BUFFER, chunk->position_buffer);
    glVertexAttribPointer(position_loc, 3, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, chunk->normal_buffer);
    glVertexAttribPointer(normal_loc, 3, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, chunk->uv_buffer);
    glVertexAttribPointer(uv_loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDrawArrays(GL_TRIANGLES, 0, chunk->faces * 6);
    glDisableVertexAttribArray(position_loc);
    glDisableVertexAttribArray(normal_loc);
    glDisableVertexAttribArray(uv_loc);
}

void draw_lines(GLuint buffer, GLuint position_loc, int size, int count) {
    glEnableVertexAttribArray(position_loc);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glVertexAttribPointer(position_loc, size, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDrawArrays(GL_LINES, 0, count);
    glDisableVertexAttribArray(position_loc);
}

void ensure_chunks(Chunk *chunks, int *chunk_count, int p, int q, int force) {
    int count = *chunk_count;
    for (int i = 0; i < count; i++) {
        Chunk *chunk = chunks + i;
        if (chunk_distance(chunk, p, q) >= DELETE_CHUNK_RADIUS) {
            map_free(&chunk->map);
            glDeleteBuffers(1, &chunk->position_buffer);
            glDeleteBuffers(1, &chunk->normal_buffer);
            glDeleteBuffers(1, &chunk->uv_buffer);
            Chunk *other = chunks + (count - 1);
            chunk->map = other->map;
            chunk->p = other->p;
            chunk->q = other->q;
            chunk->faces = other->faces;
            chunk->position_buffer = other->position_buffer;
            chunk->normal_buffer = other->normal_buffer;
            chunk->uv_buffer = other->uv_buffer;
            count--;
        }
    }
    int n = CREATE_CHUNK_RADIUS;
    for (int i = -n; i <= n; i++) {
        for (int j = -n; j <= n; j++) {
            int a = p + i;
            int b = q + j;
            if (!find_chunk(chunks, count, a, b)) {
                make_chunk(chunks + count, a, b);
                count++;
                if (!force) {
                    *chunk_count = count;
                    return;
                }
            }
        }
    }
    *chunk_count = count;
}

void _set_block(
    Chunk *chunks, int chunk_count,
    int p, int q, int x, int y, int z, int w)
{
    Chunk *chunk = find_chunk(chunks, chunk_count, p, q);
    if (chunk) {
        Map *map = &chunk->map;
        map_set(map, x, y, z, w);
        update_chunk(chunk);
    }
    /*
		Test server connection - OLD
    */
    
    //if(client_connected) {
	//	client_send(x, y, z, p, q, w);
	//}
    db_insert_block(p, q, x, y, z, w);
}

void set_block(Chunk *chunks, int chunk_count, int x, int y, int z, int w) {
    int p = floorf((float)x / CHUNK_SIZE);
    int q = floorf((float)z / CHUNK_SIZE);
    char buffer[1024];
    printf("Client -> Server: Requesting block change to %d at %d, %d, %d\n", w, x, y, z);
    snprintf(buffer, 1024, "B,%d,%d,%d,%d,%d,%d\n", p, q, x, y, z, w);
    client_send(buffer);
    _set_block(chunks, chunk_count, p, q, x, y, z, w);
    w = w ? -1 : 0;
    int p0 = x == p * CHUNK_SIZE;
    int q0 = z == q * CHUNK_SIZE;
    int p1 = x == p * CHUNK_SIZE + CHUNK_SIZE - 1;
    int q1 = z == q * CHUNK_SIZE + CHUNK_SIZE - 1;
    for (int dp = -1; dp <= 1; dp++) {
        for (int dq = -1; dq <= 1; dq++) {
            if (dp == 0 && dq == 0) continue;
            if (dp < 0 && !p0) continue;
            if (dp > 0 && !p1) continue;
            if (dq < 0 && !q0) continue;
            if (dq > 0 && !q1) continue;
            _set_block(chunks, chunk_count, p + dp, q + dq, x, y, z, w);
        }
    }
}

void on_key(GLFWwindow *window, int key, int scancode, int action, int mods) {
    if (action != GLFW_PRESS) {
        return;
    }
    if (key == GLFW_KEY_ESCAPE) {
		if (typing) {
            typing = 0;
        }
        if (exclusive) {
            exclusive = 0;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
    }
    if (key == GLFW_KEY_ENTER) {
		if (typing) {
			typing = 0;
			client_talk(typing_buffer);
			printf("\n");
		}
    }
	if (key == GLFW_KEY_BACKSPACE) {
		if (typing) {
			int n = strlen(typing_buffer);
			if (n > 0) {
				typing_buffer[n - 1] = '\0';
				printf("\b \b");
				fflush(stdout);
			}
		}
	}
    if (key == GLFW_KEY_TAB) {
        flying = !flying;
    }
    if (key == 'E') {
		block_type = block_type % 8 + 1;
		printf("Block ID: %d\n",block_type);	
	}
}

void on_char(GLFWwindow *window, unsigned int u) {
	if (typing) {
		if (u >= 32 && u < 128) {
			char c = (char)u;
			int n = strlen(typing_buffer);
			if (n < TEXT_BUFFER_SIZE - 1) {
				typing_buffer[n] = c;
				typing_buffer[n + 1] = '\0';
				printf("%c", c);
				fflush(stdout);
			}
		}
	}
	else {
		if (u == 116) { // 't'
			typing = 1;
			typing_buffer[0] = '\0';
			printf("> ");
			fflush(stdout);
		}
	}
}

void on_mouse_button(GLFWwindow *window, int button, int action, int mods) {
    if (action != GLFW_PRESS) {
        return;
    }
    if (button == 0) {
        if (exclusive) {
            left_click = 1;
        }
        else {
            exclusive = 1;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
    }
    if (button == 1) {
        if (exclusive) {
            right_click = 1;
        }
    }
}

void create_window() {
	//int width = 1024;
	//int height = 768;
	//GLFWmonitor *monitor = NULL;
	/*if (FULLSCREEN) {
		int mode_count;
		monitor = glfwGetPrimaryMonitor();
		const GLFWvidmode *modes = glfwGetVideoModes(monitor, &mode_count);
		int width = modes[mode_count - 1];
		int height = modes[mode_count - 1];
	}*/
	window = glfwCreateWindow(1024, 768, "Cubious", NULL, NULL);
}

void update_fps(FPS *fps, int show) {
    fps->frames++;
    double now = glfwGetTime();
    double elapsed = now - fps->since;
    if (elapsed >= 1) {
        int result = fps->frames / elapsed;
        fps->frames = 0;
        fps->since = now;
        if (show) {
            snprintf(message,TEXT_BUFFER_SIZE,"FPS: %d\n",result);
        }
    }
}

int main(int argc, char **argv) {
    srand(time(NULL));
    rand();
    if(argc == 2 || argc == 3) {
		char *hostname = argv[1];
		int port = atoi(argv[2]);
		db_disable();
		client_enable();
		client_connect(hostname, port);
		client_start();
	}
    if (!glfwInit()) {
        return -1;
    }
    create_window();
    if(!window) {
		glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(VSYNC);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetKeyCallback(window, on_key);
    glfwSetCharCallback(window, on_char);
    glfwSetMouseButtonCallback(window, on_mouse_button);

    #ifndef __APPLE__
        if (glewInit() != GLEW_OK) {
            return -1;
        }
    #endif

    if (db_init()) {
        return -1;
    }

    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LINE_SMOOTH);
    glLogicOp(GL_INVERT);
    glClearColor(0.53, 0.81, 0.92, 1.00);

    //GLuint vertex_array;
    //glGenVertexArrays(1, &vertex_array);
    //glBindVertexArray(vertex_array);

    GLuint texture;
    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    load_png_texture("texture.png");
	
	GLuint font;
	glGenTextures(1, &font);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, font);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	load_png_texture("font.png");
	
    //GLuint block_program; 
    GLuint block_program = load_program("shaders/block_vertex.glsl", "shaders/block_fragment.glsl");
    GLuint matrix_loc = glGetUniformLocation(block_program, "matrix");
    GLuint camera_loc = glGetUniformLocation(block_program, "camera");
    GLuint sampler_loc = glGetUniformLocation(block_program, "sampler");
    GLuint timer_loc = glGetUniformLocation(block_program, "timer");
    GLuint position_loc = glGetAttribLocation(block_program, "position");
    GLuint normal_loc = glGetAttribLocation(block_program, "normal");
    GLuint uv_loc = glGetAttribLocation(block_program, "uv");

	//GLuint line_program;
    GLuint line_program = load_program("shaders/line_vertex.glsl", "shaders/line_fragment.glsl");
    GLuint line_matrix_loc = glGetUniformLocation(line_program, "matrix");
    GLuint line_position_loc = glGetAttribLocation(line_program, "position");

	GLuint text_program = load_program("shaders/text_vertex.glsl", "shaders/text_fragment.glsl");
	GLuint text_matrix_loc = glGetUniformLocation(text_program, "matrix");
	GLuint text_sampler_loc = glGetUniformLocation(text_program, "sampler");
	GLuint text_position_loc = glGetAttribLocation(text_program, "position");
	GLuint text_uv_loc = glGetAttribLocation(text_program, "uv");

	

    Chunk chunks[MAX_CHUNKS];
    int chunk_count = 0;

    FPS fps = {0, 0};
    float matrix[16];
    float x = (rand_double() - 0.5) * 10000;
    float z = (rand_double() - 0.5) * 10000;
    float y = 0;
    float dy = 0;
    float rx = 0;
    float ry = 0;
    double px = 0;
    double py = 0;
	int width, height;
    glfwGetWindowSize(window, &width, &height);
    
    int loaded = db_load_state(&x, &y, &z, &rx, &ry);
    // Check if connected to a server or not
    if(!get_client_enabled()) {
		ensure_chunks(chunks, &chunk_count,
			floorf(roundf(x) / CHUNK_SIZE),
			floorf(roundf(z) / CHUNK_SIZE), 1);
	}
    if (!loaded) {
        y = highest_block(chunks, chunk_count, x, z) + 2;
    }

    glfwGetCursorPos(window, &px, &py);
    double previous = glfwGetTime();
    
    /*
		Test server connection - OLD
    */
    //client_connect(address, port);
    
    while (!glfwWindowShouldClose(window)) {
        update_fps(&fps, SHOW_FPS);
        double now = glfwGetTime();
        double dt = MIN(now - previous, 0.2);
        previous = now;

        if (exclusive) {
            double mx, my;
            glfwGetCursorPos(window, &mx, &my);
            float m = 0.0025;
            rx += (mx - px) * m;
            ry -= (my - py) * m;
            if (rx < 0) {
                rx += RADIANS(360);
            }
            if (rx >= RADIANS(360)){
                rx -= RADIANS(360);
            }
            ry = MAX(ry, -RADIANS(90));
            ry = MIN(ry, RADIANS(90));
            px = mx;
            py = my;
        } else {
			glfwGetCursorPos(window, &px, &py);
        }

        if (left_click) {
            left_click = 0;
            int hx, hy, hz;
            if (hit_test(chunks, chunk_count, 0, x, y, z, rx, ry,
                &hx, &hy, &hz))
            {
                if (hy > 0) {
                    set_block(chunks, chunk_count, hx, hy, hz, 0);
                }
            }
        }

        if (right_click) {
            right_click = 0;
            int hx, hy, hz;
            int hw = hit_test(chunks, chunk_count, 1, x, y, z, rx, ry,
                &hx, &hy, &hz);
            if (is_obstacle(hw))
            {
                if (!player_intersects_block(2, x, y, z, hx, hy, hz)) {
                    set_block(chunks, chunk_count, hx, hy, hz, block_type);
                }
            }
        }

        int sz = 0;
        int sx = 0;
		if(!typing) {
			ortho = glfwGetKey(window,GLFW_KEY_LEFT_SHIFT);
			if (glfwGetKey(window,'Q')) break;
			if (glfwGetKey(window,'W')) sz--;
			if (glfwGetKey(window,'S')) sz++;
			if (glfwGetKey(window,'A')) sx--;
			if (glfwGetKey(window,'D')) sx++;
			if (dy == 0 && glfwGetKey(window,GLFW_KEY_SPACE)) {
				dy = 8;
			}
		}
        float vx, vy, vz;
        get_motion_vector(flying, sz, sx, rx, ry, &vx, &vy, &vz);
        float speed = flying ? 20 : 5;
        int step = 8;
        float ut = dt / step;
        vx = vx * ut * speed;
        vy = vy * ut * speed;
        vz = vz * ut * speed;
        for (int i = 0; i < step; i++) {
            if (flying) {
                dy = 0;
            }
            else {
                dy -= ut * 25;
                dy = MAX(dy, -250);
            }
            x += vx;
            y += vy + dy * ut;
            z += vz;
            if (collide(chunks, chunk_count, 2, &x, &y, &z)) {
                dy = 0;
            }
        }

		// If connected to a server, secure chunks and update
		//   from the server, not local database
		char buffer[1024];
		while (client_recv(buffer)) {
			if (buffer[0] == 'U') {
				sscanf(buffer, "U,%*d,%f,%f,%f", &x, &y, &z);
				ensure_chunks(chunks, &chunk_count,
					floorf(roundf(x) / CHUNK_SIZE),
					floorf(roundf(z) / CHUNK_SIZE), 1);
				y = highest_block(chunks, chunk_count, x, z) + 2;
			}
			if(buffer[0] == 'B') {
				int bp, bq, bx, by, bz, bw;
				sscanf(buffer, "B,%d,%d,%d,%d,%d,%d", &bp, &bq, &bx, &by, &bz, &bw);
				printf("Server -> Client: Block update at [%d, %d, %d]\n", bx, by, bz);
				set_block(chunks, chunk_count, bx, by, bz, bw);
			}
			if (buffer[0] == 'T' && buffer[1] == ',') {
                char *text = buffer + 2;
                printf("%s\n", text);
                snprintf(message, TEXT_BUFFER_SIZE, "%s", text);
            }
		}

        int p = floorf(roundf(x) / CHUNK_SIZE);
        int q = floorf(roundf(z) / CHUNK_SIZE);
        ensure_chunks(chunks, &chunk_count, p, q, 0);

        update_matrix_3d(matrix, x, y, z, rx, ry);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // render chunks
        glUseProgram(block_program);
        glUniformMatrix4fv(matrix_loc, 1, GL_FALSE, matrix);
        glUniform3f(camera_loc, x, y, z);
        glUniform1i(sampler_loc, 0);
        glUniform1f(timer_loc, glfwGetTime());
        for (int i = 0; i < chunk_count; i++) {
            Chunk *chunk = chunks + i;
            if (chunk_distance(chunk, p, q) > RENDER_CHUNK_RADIUS) {
                continue;
            }
            if (!chunk_visible(chunk, matrix)) {
                continue;
            }
            draw_chunk(chunk, position_loc, normal_loc, uv_loc);
        }

        // render focused block wireframe
        int hx, hy, hz;
        int hw = hit_test(chunks, chunk_count, 0, x, y, z, rx, ry, &hx, &hy, &hz);
        if (is_obstacle(hw)) {
            glUseProgram(line_program);
            glLineWidth(1);
            glEnable(GL_COLOR_LOGIC_OP);
            glUniformMatrix4fv(line_matrix_loc, 1, GL_FALSE, matrix);
            GLuint cube_buffer = make_cube_buffer(hx, hy, hz, 0.51);
            draw_lines(cube_buffer, line_position_loc, 3, 48);
            glDeleteBuffers(1, &cube_buffer);
            glDisable(GL_COLOR_LOGIC_OP);
        }

        update_matrix_2d(matrix);

        // render crosshairs
        glUseProgram(line_program);
        glLineWidth(4);
        glEnable(GL_COLOR_LOGIC_OP);
        glUniformMatrix4fv(line_matrix_loc, 1, GL_FALSE, matrix);
        GLuint line_buffer = make_line_buffer();
        draw_lines(line_buffer, line_position_loc, 2, 4);
        glDeleteBuffers(1, &line_buffer);
        glDisable(GL_COLOR_LOGIC_OP);

        glfwSwapBuffers(window);
        glfwPollEvents();
        
        // render text
        update_matrix_2d(matrix);
		glUseProgram(text_program);
        glUniformMatrix4fv(text_matrix_loc, 1, GL_FALSE, matrix);
        glUniform1i(text_sampler_loc, 1);
        char text_buffer[1024];
		float ty = height - 12;
        snprintf(
            text_buffer, 1024, "%d, %d, %.2f, %.2f, %.2f [%d]",
            p, q, x, y, z, chunk_count);
        print(
            text_position_loc, text_uv_loc,
            6, ty, 6, text_buffer);
        print(
            text_position_loc, text_uv_loc,
            6, ty, 6, message);
        if(strlen(message)) {
				ty -= 24;
				print(text_position_loc, text_uv_loc,
					6, ty, 6, message);
		}
		if(typing) {
			ty -= 24;
			snprintf(text_buffer, 1024, "> %s", typing_buffer);
			print(text_position_loc, text_uv_loc,
				6, ty, 6, text_buffer);
		}
    
    }
    client_stop();
    db_save_state(x, y, z, rx, ry);
    db_close();
    glfwTerminate();
    return 0;
}

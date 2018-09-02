#include "Game.hpp"

#include "gl_errors.hpp" //helper for dumpping OpenGL error messages
#include "read_chunk.hpp" //helper for reading a vector of structures from a file
#include "data_path.hpp" //helper to get paths relative to executable

#include <glm/gtc/type_ptr.hpp>

#include <iostream>
#include <fstream>
#include <map>
#include <cstddef>
#include <random>
#include <algorithm>

//helper defined later; throws if shader compilation fails:
static GLuint compile_shader(GLenum type, std::string const &source);

Game::Game() {
	{ //create an opengl program to perform sun/sky (well, directional+hemispherical) lighting:
		GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER,
			"#version 330\n"
			"uniform mat4 object_to_clip;\n"
			"uniform mat4x3 object_to_light;\n"
			"uniform mat3 normal_to_light;\n"
			"layout(location=0) in vec4 Position;\n" //note: layout keyword used to make sure that the location-0 attribute is always bound to something
			"in vec3 Normal;\n"
			"in vec4 Color;\n"
			"out vec3 position;\n"
			"out vec3 normal;\n"
			"out vec4 color;\n"
			"void main() {\n"
			"	gl_Position = object_to_clip * Position;\n"
			"	position = object_to_light * Position;\n"
			"	normal = normal_to_light * Normal;\n"
			"	color = Color;\n"
			"}\n"
		);

		GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER,
			"#version 330\n"
			"uniform vec3 sun_direction;\n"
			"uniform vec3 sun_color;\n"
			"uniform vec3 sky_direction;\n"
			"uniform vec3 sky_color;\n"
			"in vec3 position;\n"
			"in vec3 normal;\n"
			"in vec4 color;\n"
			"out vec4 fragColor;\n"
			"void main() {\n"
			"	vec3 total_light = vec3(0.0, 0.0, 0.0);\n"
			"	vec3 n = normalize(normal);\n"
			"	{ //sky (hemisphere) light:\n"
			"		vec3 l = sky_direction;\n"
			"		float nl = 0.5 + 0.5 * dot(n,l);\n"
			"		total_light += nl * sky_color;\n"
			"	}\n"
			"	{ //sun (directional) light:\n"
			"		vec3 l = sun_direction;\n"
			"		float nl = max(0.0, dot(n,l));\n"
			"		total_light += nl * sun_color;\n"
			"	}\n"
			"	fragColor = vec4(color.rgb * total_light, color.a);\n"
			"}\n"
		);

		simple_shading.program = glCreateProgram();
		glAttachShader(simple_shading.program, vertex_shader);
		glAttachShader(simple_shading.program, fragment_shader);
		//shaders are reference counted so this makes sure they are freed after program is deleted:
		glDeleteShader(vertex_shader);
		glDeleteShader(fragment_shader);

		//link the shader program and throw errors if linking fails:
		glLinkProgram(simple_shading.program);
		GLint link_status = GL_FALSE;
		glGetProgramiv(simple_shading.program, GL_LINK_STATUS, &link_status);
		if (link_status != GL_TRUE) {
			std::cerr << "Failed to link shader program." << std::endl;
			GLint info_log_length = 0;
			glGetProgramiv(simple_shading.program, GL_INFO_LOG_LENGTH, &info_log_length);
			std::vector< GLchar > info_log(info_log_length, 0);
			GLsizei length = 0;
			glGetProgramInfoLog(simple_shading.program, GLsizei(info_log.size()), &length, &info_log[0]);
			std::cerr << "Info log: " << std::string(info_log.begin(), info_log.begin() + length);
			throw std::runtime_error("failed to link program");
		}
	}

	{ //read back uniform and attribute locations from the shader program:
		simple_shading.object_to_clip_mat4 = glGetUniformLocation(simple_shading.program, "object_to_clip");
		simple_shading.object_to_light_mat4x3 = glGetUniformLocation(simple_shading.program, "object_to_light");
		simple_shading.normal_to_light_mat3 = glGetUniformLocation(simple_shading.program, "normal_to_light");

		simple_shading.sun_direction_vec3 = glGetUniformLocation(simple_shading.program, "sun_direction");
		simple_shading.sun_color_vec3 = glGetUniformLocation(simple_shading.program, "sun_color");
		simple_shading.sky_direction_vec3 = glGetUniformLocation(simple_shading.program, "sky_direction");
		simple_shading.sky_color_vec3 = glGetUniformLocation(simple_shading.program, "sky_color");

		simple_shading.Position_vec4 = glGetAttribLocation(simple_shading.program, "Position");
		simple_shading.Normal_vec3 = glGetAttribLocation(simple_shading.program, "Normal");
		simple_shading.Color_vec4 = glGetAttribLocation(simple_shading.program, "Color");
	}

	struct Vertex {
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::u8vec4 Color;
	};
	static_assert(sizeof(Vertex) == 28, "Vertex should be packed.");

	{ //load mesh data from a binary blob:
		std::ifstream blob(data_path("meshes.blob"), std::ios::binary);
		//The blob will be made up of three chunks:
		// the first chunk will be vertex data (interleaved position/normal/color)
		// the second chunk will be characters
		// the third chunk will be an index, mapping a name (range of characters) to a mesh (range of vertex data)

		//read vertex data:
		std::vector< Vertex > vertices;
		read_chunk(blob, "dat0", &vertices);

		//read character data (for names):
		std::vector< char > names;
		read_chunk(blob, "str0", &names);

		//read index:
		struct IndexEntry {
			uint32_t name_begin;
			uint32_t name_end;
			uint32_t vertex_begin;
			uint32_t vertex_end;
		};
		static_assert(sizeof(IndexEntry) == 16, "IndexEntry should be packed.");

		std::vector< IndexEntry > index_entries;
		read_chunk(blob, "idx0", &index_entries);

		if (blob.peek() != EOF) {
			std::cerr << "WARNING: trailing data in meshes file." << std::endl;
		}

		//upload vertex data to the graphics card:
		glGenBuffers(1, &meshes_vbo);
		glBindBuffer(GL_ARRAY_BUFFER, meshes_vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * vertices.size(), vertices.data(), GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);

		//create map to store index entries:
		std::map< std::string, Mesh > index;
		for (IndexEntry const &e : index_entries) {
			if (e.name_begin > e.name_end || e.name_end > names.size()) {
				throw std::runtime_error("invalid name indices in index.");
			}
			if (e.vertex_begin > e.vertex_end || e.vertex_end > vertices.size()) {
				throw std::runtime_error("invalid vertex indices in index.");
			}
			Mesh mesh;
			mesh.first = e.vertex_begin;
			mesh.count = e.vertex_end - e.vertex_begin;
			auto ret = index.insert(std::make_pair(
				std::string(names.begin() + e.name_begin, names.begin() + e.name_end),
				mesh));
			if (!ret.second) {
				throw std::runtime_error("duplicate name in index.");
			}
		}

		//look up into index map to extract meshes:
		auto lookup = [&index](std::string const &name) -> Mesh {
			auto f = index.find(name);
			if (f == index.end()) {
				throw std::runtime_error("Mesh named '" + name + "' does not appear in index.");
			}
			return f->second;
		};
		wall_mesh = lookup("Wall");
		floor_mesh = lookup("Floor");
		player_mesh = lookup("Player");
		goop_mesh = lookup("Goop");
		checkpoint_mesh = lookup("Checkpoint");
		checkpoint_collected_mesh = lookup("CheckpointCollected");
		goal_mesh = lookup("Goal");
	}

	{ //create vertex array object to hold the map from the mesh vertex buffer to shader program attributes:
		glGenVertexArrays(1, &meshes_for_simple_shading_vao);
		glBindVertexArray(meshes_for_simple_shading_vao);
		glBindBuffer(GL_ARRAY_BUFFER, meshes_vbo);
		//note that I'm specifying a 3-vector for a 4-vector attribute here, and this is okay to do:
		glVertexAttribPointer(simple_shading.Position_vec4, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, Position));
		glEnableVertexAttribArray(simple_shading.Position_vec4);
		if (simple_shading.Normal_vec3 != -1U) {
			glVertexAttribPointer(simple_shading.Normal_vec3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, Normal));
			glEnableVertexAttribArray(simple_shading.Normal_vec3);
		}
		if (simple_shading.Color_vec4 != -1U) {
			glVertexAttribPointer(simple_shading.Color_vec4, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, Color));
			glEnableVertexAttribArray(simple_shading.Color_vec4);
		}
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	GL_ERRORS();

	//----------------
	//set up game board with meshes and rolls:
	board_meshes.resize(board_size.x * board_size.y, nullptr);
	create_board();
}

Game::~Game() {
	glDeleteVertexArrays(1, &meshes_for_simple_shading_vao);
	meshes_for_simple_shading_vao = -1U;

	glDeleteBuffers(1, &meshes_vbo);
	meshes_vbo = -1U;

	glDeleteProgram(simple_shading.program);
	simple_shading.program = -1U;

	GL_ERRORS();
}

bool Game::handle_event(SDL_Event const &evt, glm::uvec2 window_size) {
	//ignore any keys that are the result of automatic key repeat:
	if (evt.type == SDL_KEYDOWN && evt.key.repeat) {
		return false;
	}
	//move player on L/R/U/D press:
	if (evt.type == SDL_KEYDOWN && evt.key.repeat == 0) {
		if (evt.key.keysym.scancode == SDL_SCANCODE_LEFT) {
			move_player(-1, 0);
			return true;
		} else if (evt.key.keysym.scancode == SDL_SCANCODE_RIGHT) {
			move_player( 1, 0);
			return true;
		} else if (evt.key.keysym.scancode == SDL_SCANCODE_UP) {
			move_player( 0, 1);
			return true;
		} else if (evt.key.keysym.scancode == SDL_SCANCODE_DOWN) {
			move_player( 0,-1);
			return true;
		} else if (evt.key.keysym.scancode == SDL_SCANCODE_BACKSPACE) {
			//backspace: give up
			if (checkpoints > 0) checkpoints -= 1;
			create_board();
			return true;
		} else if (evt.key.keysym.scancode == SDL_SCANCODE_SPACE) {
			//space (on goal): next level
			if (won) {
				create_board();
			}
			return true;
		}
	}
	return false;
}

void Game::update(float elapsed) {
}

void Game::draw(glm::uvec2 drawable_size) {
	//Set up a transformation matrix to fit the board in the window:
	glm::mat4 world_to_clip;
	{
		float aspect = float(drawable_size.x) / float(drawable_size.y);

		//weird shear transform that will be applied during projection for artistic reasons:
		glm::mat4 shear = glm::mat4(
			1.0f, 0.0f, 0.0f, 0.0f,
			-0.07f, 0.9f, 0.0f, 0.0f,
			 0.0f, 0.2f, 1.0f, 0.0f,
			 0.0f, 0.0f, 0.0f, 1.0f
		);

		//figure out bounding box of board when transformed by shear:
		glm::vec2 board_min = glm::vec2(std::numeric_limits< float >::infinity());
		glm::vec2 board_max = glm::vec2(-std::numeric_limits< float >::infinity());
		for (float cx : { 0.5f, board_size.x - 0.5f }) {
			for (float cy : { 0.5f, board_size.y - 0.5f }) {
				for (float cz : { 0.0f, 1.0f }) {
					glm::vec2 pt = glm::vec2(shear * glm::vec4(cx, cy, cz, 1.0f));
					board_min = glm::min(board_min, pt);
					board_max = glm::max(board_max, pt);
				}
			}
		}

		//want scale such that [board_min,board_max] * scale fits in [-aspect,aspect]x[-1.0,1.0] screen box:
		float scale = glm::min(
			2.0f * aspect / float(board_max.x - board_min.x),
			2.0f / float(board_max.y - board_min.y)
		);

		//center of board will be placed at center of screen:
		glm::vec2 center = 0.5f * (board_max + board_min);

		//NOTE: glm matrices are specified in column-major order
		world_to_clip = glm::mat4(
			scale / aspect, 0.0f, 0.0f, 0.0f,
			0.0f, scale, 0.0f, 0.0f,
			0.0f, 0.0f,-1.0f, 0.0f,
			-(scale / aspect) * center.x, -scale * center.y, 0.0f, 1.0f
		) * shear ;
	}

	//set up graphics pipeline to use data from the meshes and the simple shading program:
	glBindVertexArray(meshes_for_simple_shading_vao);
	glUseProgram(simple_shading.program);

	glUniform3fv(simple_shading.sun_color_vec3, 1, glm::value_ptr(glm::vec3(0.81f, 0.81f, 0.76f)));
	glUniform3fv(simple_shading.sun_direction_vec3, 1, glm::value_ptr(glm::normalize(glm::vec3(-0.2f, 0.2f, 1.0f))));
	glUniform3fv(simple_shading.sky_color_vec3, 1, glm::value_ptr(glm::vec3(0.2f, 0.2f, 0.3f)));
	glUniform3fv(simple_shading.sky_direction_vec3, 1, glm::value_ptr(glm::vec3(0.0f, 1.0f, 0.0f)));

	//helper function to draw a given mesh with a given transformation:
	auto draw_mesh = [&](Mesh const &mesh, glm::mat4 const &object_to_world) {
		//set up the matrix uniforms:
		if (simple_shading.object_to_clip_mat4 != -1U) {
			glm::mat4 object_to_clip = world_to_clip * object_to_world;
			glUniformMatrix4fv(simple_shading.object_to_clip_mat4, 1, GL_FALSE, glm::value_ptr(object_to_clip));
		}
		if (simple_shading.object_to_light_mat4x3 != -1U) {
			glUniformMatrix4x3fv(simple_shading.object_to_light_mat4x3, 1, GL_FALSE, glm::value_ptr(object_to_world));
		}
		if (simple_shading.normal_to_light_mat3 != -1U) {
			//NOTE: if there isn't any non-uniform scaling in the object_to_world matrix, then the inverse transpose is the matrix itself, and computing it wastes some CPU time:
			glm::mat3 normal_to_world = glm::inverse(glm::transpose(glm::mat3(object_to_world)));
			glUniformMatrix3fv(simple_shading.normal_to_light_mat3, 1, GL_FALSE, glm::value_ptr(normal_to_world));
		}

		//draw the mesh:
		glDrawArrays(GL_TRIANGLES, mesh.first, mesh.count);
	};

	for (uint32_t y = 0; y < board_size.y; ++y) {
		for (uint32_t x = 0; x < board_size.x; ++x) {
			draw_mesh(*board_meshes[y*board_size.x+x],
				glm::mat4(
					1.0f, 0.0f, 0.0f, 0.0f,
					0.0f, 1.0f, 0.0f, 0.0f,
					0.0f, 0.0f, 1.0f, 0.0f,
					x+0.5f, y+0.5f, 0.0f, 1.0f
				)
			);
			if (goal_meshes[y*board_size.x+x]) {
				draw_mesh(*goal_meshes[y*board_size.x+x],
					glm::mat4(
						1.0f, 0.0f, 0.0f, 0.0f,
						0.0f, 1.0f, 0.0f, 0.0f,
						0.0f, 0.0f, 1.0f, 0.0f,
						x+0.5f, y+0.5f, 0.0f, 1.0f
					)
				);
			}
		}
	}
	draw_mesh(player_mesh,
		glm::mat4(
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			player.x+0.5f, player.y+0.5f, 0.0f, 1.0f
		)
	);


	glUseProgram(0);

	GL_ERRORS();
}



//create and return an OpenGL vertex shader from source:
static GLuint compile_shader(GLenum type, std::string const &source) {
	GLuint shader = glCreateShader(type);
	GLchar const *str = source.c_str();
	GLint length = GLint(source.size());
	glShaderSource(shader, 1, &str, &length);
	glCompileShader(shader);
	GLint compile_status = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compile_status);
	if (compile_status != GL_TRUE) {
		std::cerr << "Failed to compile shader." << std::endl;
		GLint info_log_length = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_log_length);
		std::vector< GLchar > info_log(info_log_length, 0);
		GLsizei length = 0;
		glGetShaderInfoLog(shader, GLsizei(info_log.size()), &length, &info_log[0]);
		std::cerr << "Info log: " << std::string(info_log.begin(), info_log.begin() + length);
		glDeleteShader(shader);
		throw std::runtime_error("Failed to compile shader.");
	}
	return shader;
}


void Game::create_board() {
	static std::mt19937 mt(0xbead1234);

	//remove everything:
	board_meshes.assign(board_size.x * board_size.y, &wall_mesh);
	for (uint32_t x = 1; x + 1 < board_size.x; ++x) {
		for (uint32_t y = 1; y + 1 < board_size.y; ++y) {
			board_meshes[y*board_size.x + x] = &floor_mesh;
		}
	}
	goal_meshes.assign(board_size.x * board_size.y, nullptr);

	auto random_board_position = [&,this](){
		return glm::uvec2(
			mt() % (board_size.x-2) + 1,
			mt() % (board_size.y-2) + 1
		);
	};

	{ //place some random walls:
		uint32_t walls = (mt() % 8) + 2;
		for (uint32_t w = 0; w < walls; ++w) {
			//note: may end up placing walls atop other walls, but that's fine
			glm::uvec2 pos = random_board_position();
			if (pos == player) continue; //shouldn't place walls on player, though.
			board_meshes[pos.y*board_size.x+pos.x] = &wall_mesh;
		}
	}

	{ //place some random goops:
		uint32_t goops = (mt() % 4);
		for (uint32_t g = 0; g < goops; ++g) {
			glm::uvec2 pos = random_board_position();
			if (board_meshes[pos.y*board_size.x+pos.x] != &wall_mesh) {
				goal_meshes[pos.y*board_size.x+pos.x] = &goop_mesh;
			}
		}
	}

	//try to generate several goals:
	uint32_t goals = 0;
	glm::uvec2 prev_goal = player;
	while (goals < 2) {
		//run some random walks to check where player is likely to end up starting at previous goal:
		std::vector< uint32_t > board_counts(board_size.x * board_size.y, 0);
		for (uint32_t iter = 0; iter < 100; ++iter) {
			glm::vec2 at = prev_goal;
			for (uint32_t step = 0; step < 20; ++step) {
				static const glm::ivec2 directions[4] = {
					glm::ivec2(-1,0), glm::ivec2(1,0),
					glm::ivec2(0,-1), glm::ivec2(0,1)
				};
				glm::vec2 d = directions[mt() % 4];
				while (board_meshes[(at.y+d.y)*board_size.x+(at.x+d.x)] != &wall_mesh) {
					at += d;
					if (goal_meshes[at.y*board_size.x+at.x] == &goop_mesh) break;
				}
				board_counts[at.y*board_size.x+at.x] += 1;
			}
		}
		//make a list of possible checkpoint cells based on likelihoods:
		std::vector< glm::uvec2 > possible_cells;
		for (uint32_t y = 0; y < board_size.y; ++y) {
			for (uint32_t x = 0; x < board_size.x; ++x) {
				if (x == player.x && y == player.y) continue; //don't place checkpoint at player
				if (goal_meshes[y*board_size.x+x] != nullptr) continue; //don't overlap goals
				if (board_counts[y*board_size.x+x] > 0) {
					possible_cells.emplace_back(x,y);
				}
			}
		}
		//ran out of possible goal locations:
		if (possible_cells.empty()) break;

		//now sort list based on counts (smaller counts == harder):
		std::stable_sort(possible_cells.begin(), possible_cells.end(), [&](glm::uvec2 a, glm::uvec2 b) {
			return board_counts[a.y*board_size.x+a.x] < board_counts[b.y*board_size.x+b.x];
		});

		//pick one for the goal:
		//limit to picking cells in the highest 25% of difficulty:
		uint32_t limit = std::max< uint32_t >(1, possible_cells.size() / 4);
		//extend limit to all cells with the same count:
		while (limit + 1 < possible_cells.size() && board_counts[possible_cells[limit].y*board_size.x+possible_cells[limit].x] == board_counts[possible_cells[limit+1].y*board_size.x+possible_cells[limit+1].x]) ++limit;
		glm::uvec2 g = possible_cells[mt() % limit];

		assert(goal_meshes[g.y*board_size.x+g.x] == nullptr);
		goal_meshes[g.y*board_size.x+g.x] = &checkpoint_mesh;
		++goals;
		prev_goal = g;
	}

	if (goals == 0) {
		//failed to generate a board with at least one goal, so retry:
		create_board();
		return;
	}

	//turn the last goal into the main goal:
	goal_meshes[prev_goal.y*board_size.x+prev_goal.x] = &goal_mesh;

}

void Game::move_player(int32_t dx, int32_t dy) {
	//step player until it is on goop or next tile is a wall
	assert(player.x >= 1 && player.x + 1 < board_size.x);
	assert(player.y >= 1 && player.y + 1 < board_size.y);
	while (board_meshes[(player.y+dy)*board_size.x+(player.x+dx)] != &wall_mesh) {
		player.x += dx;
		player.y += dy;
		//did the player step onto goop?
		if (goal_meshes[player.y*board_size.x+player.x] == &goop_mesh) break;
	}

	//did the player gather a checkpoint?
	if (goal_meshes[player.y*board_size.x+player.x] == &checkpoint_mesh) {
		goal_meshes[player.y*board_size.x+player.x] = &checkpoint_collected_mesh;
		checkpoints += 1;
	}

	won = (goal_meshes[player.y*board_size.x+player.x] == &goal_mesh);
}

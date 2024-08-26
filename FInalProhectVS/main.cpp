#define STB_IMAGE_IMPLEMENTATION
#define CPPHTTPLIB_OPENSSL_SUPPORT

#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <map>
#include <atomic>
#include <set>
#include <fstream>
#include <filesystem>
#include <glad/glad.h>
#include <GLFW/glfw3.h> 
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <stb_image.h>
#include <json.hpp>
#include <httplib.h>
#include <thread_safe_queue.h>
#include <openssl/err.h>
#include <openssl/ssl.h>


std::string GetExecutablePath() {//NEEDS TO BE AT THE TOP OF HERE DON'T MOVE ME
    return std::filesystem::current_path().string();
}

namespace fs = std::filesystem;
using json = nlohmann::json;

#define REGULAR_FONT "include/ImGui/misc/fonts/Karla-Regular.ttf"
#define SPECIAL_FONT "include/ImGui/misc/fonts/Pacifico-Regular.ttf"

#define USER_DIRECTORY "./users/"
#define FONT_SIZE 24.0f

typedef struct Movie {
    std::string title;
    std::string producer;
    std::string release_year;
    std::string runtime;
    std::vector<std::string> genres;
    std::vector<std::string> cast;
    std::string poster_url;
    GLuint texture_id = 0;
    std::string rating;
    std::string votes;
} Movie;

struct ImageData {
    unsigned char* data = nullptr;
    int width = 0;
    int height = 0;
    int channels = 0;
    GLuint texture_id = 0;
};

// Global variables of the pro
std::mutex mtx;
std::condition_variable cv;
std::queue<std::string> image_queue;
std::atomic<bool> image_thread_running(true);
std::map<std::string, ImageData> textureMap;
std::vector<Movie> watch_list;
std::set<std::string> watch_list_titles;
GLFWwindow* window;
std::string current_user;
bool first_run = true;
char title_input[256] = "";
char year_input[5] = "";
bool movie_not_found = false;
bool connection_error = false;
Movie selected_movie;
std::string image_url;
int selected_movie_index = -1;
std::vector<Movie> movie_list;
std::atomic<bool> search_in_progress(false);
ThreadSafeQueue<Movie> movie_queue;
bool show_not_in_list_message = false;
std::atomic<bool> fetch_in_progress(false);
std::thread fetch_thread;


// Movie
bool FetchMovieInfo(Movie& movie) {
    std::string api_key = "766745cb"; // Replace with your OMDb API key
    std::string encoded_title = httplib::detail::encode_url(movie.title);
    std::string url = "/?t=" + encoded_title + "&y=" + movie.release_year + "&apikey=" + api_key;

    httplib::Client cli("https://www.omdbapi.com");
    auto res = cli.Get(url);

    if (!res) {
        connection_error = true;
        return false;
    }

    if (res->status == 200) {
        json response = json::parse(res->body);
        if (response["Response"] == "True") {
            movie.title = response.value("Title", movie.title);
            movie.producer = response.value("Director", "Unknown");
            movie.release_year = response.value("Year", movie.release_year);
            movie.runtime = response.value("Runtime", "Unknown");
            movie.rating = response.value("imdbRating", "N/A");  // New line
            movie.votes = response.value("imdbVotes", "N/A");    // New line

            // Handle Genre
            movie.genres.clear();
            std::string genre_str = response.value("Genre", "");
            std::istringstream ss(genre_str);
            std::string genre;
            while (std::getline(ss, genre, ',')) {
                movie.genres.push_back(genre);
            }

            // Handle Cast
            movie.cast.clear();
            std::string cast_str = response.value("Actors", "");
            std::istringstream cast_ss(cast_str);
            std::string actor;
            while (std::getline(cast_ss, actor, ',')) {
                movie.cast.push_back(actor);
            }

            // Handle Poster
            if (response.contains("Poster") && response["Poster"] != "N/A") {
                image_url = response["Poster"].get<std::string>();
                movie.poster_url = image_url;
            }
            else {
                image_url = "";
                movie.poster_url = "";
            }

            connection_error = false;
            return true;
        }
    }
    connection_error = false;
    return false;
}
void FetchMovieList(const std::string& title, const std::string& year) {
    std::string api_key = "766745cb"; // Make sure this is your correct API key
    std::string encoded_title = httplib::detail::encode_url(title);
    std::string url = "/?s=" + encoded_title + "&type=movie&apikey=" + api_key;

    httplib::Client cli("https://www.omdbapi.com");
    auto res = cli.Get(url);

    if (!res) {
        connection_error = true;
        movie_queue.setFinished();
        return;
    }

    if (res->status == 200) {
        json response = json::parse(res->body);
        if (response["Response"] == "True" && response.contains("Search")) {
            for (const auto& item : response["Search"]) {
                Movie movie;
                movie.title = item.value("Title", "Unknown");
                movie.release_year = item.value("Year", "Unknown");
                movie.poster_url = item.value("Poster", "");

                // Apply year filter here if specified
                if (year.empty() || movie.release_year.find(year) != std::string::npos) {
                    movie_queue.push(movie);
                }
            }
            connection_error = false;
        }
        else {
            // No movies found or error in response
            connection_error = false; // It's not a connection error, just no results
        }
    }
    else {
        connection_error = true;
    }

    movie_queue.setFinished();
}
void FetchMovieDetails(int index) {
    Movie temp_movie = movie_list[index];
    bool fetch_success = FetchMovieInfo(temp_movie);
    if (fetch_success) {
        std::lock_guard<std::mutex> lock(mtx);
        movie_list[index] = temp_movie;
        if (index == selected_movie_index) {
            selected_movie = temp_movie;
            if (!temp_movie.poster_url.empty()) {
                image_url = temp_movie.poster_url;
                if (textureMap.find(image_url) == textureMap.end()) {
                    image_queue.push(image_url);
                    cv.notify_one();
                }
            }
        }
    }
    fetch_in_progress.store(false);
}

// Image
GLuint LoadWelcomeImage(const char* filename)
{
    int width, height, channels;
    unsigned char* data = stbi_load(filename, &width, &height, &channels, 0);
    if (!data) {
        std::cerr << "Failed to load welcome image: " << filename << std::endl;
        std::cerr << "STB Error: " << stbi_failure_reason() << std::endl;
        return 0;
    }

    GLuint texture_id;
    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, channels == 4 ? GL_RGBA : GL_RGB, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);

    stbi_image_free(data);
    return texture_id;
}
void LoadImageFromUrl(const std::string& url) {
    httplib::SSLClient cli("m.media-amazon.com");
    cli.set_follow_location(true);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(10);

    httplib::Headers headers = {
            {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36"}
    };

    std::string path = url.substr(url.find("/images"));

    auto res = cli.Get(path, headers);
    if (res && res->status == 200) {
        int width, height, channels;
        unsigned char* data = stbi_load_from_memory(
            reinterpret_cast<const unsigned char*>(res->body.c_str()),
            (int)res->body.size(), &width, &height, &channels, 0
        );

        if (data) {
            std::unique_lock<std::mutex> lock(mtx);
            textureMap[url] = { data, width, height, channels, 0 };
            glfwPostEmptyEvent();
        }
        else {
            std::cerr << "Failed to decode image data. STB Error: " << stbi_failure_reason() << std::endl;
        }
    }
    else {
        std::cerr << "Failed to download image. Status: " << (res ? res->status : 0) << std::endl;
    }
}
void ImageLoadingThread() {
    while (image_thread_running) {
        std::unique_lock<std::mutex> lock(mtx);
        if (cv.wait_for(lock, std::chrono::seconds(1), [] { return !image_queue.empty() || !image_thread_running; })) {
            if (!image_thread_running) break;

            std::string url = image_queue.front();
            image_queue.pop();
            lock.unlock();

            LoadImageFromUrl(url);
        }
    }
}
void CreateTexture(const std::string& url) {
    if (textureMap.find(url) != textureMap.end()) {
        ImageData& imageData = textureMap[url];
        if (imageData.texture_id == 0 && imageData.data != nullptr) {
            glGenTextures(1, &imageData.texture_id);
            glBindTexture(GL_TEXTURE_2D, imageData.texture_id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, imageData.width, imageData.height, 0,
                imageData.channels == 4 ? GL_RGBA : GL_RGB, GL_UNSIGNED_BYTE, imageData.data);
            glBindTexture(GL_TEXTURE_2D, 0);
            stbi_image_free(imageData.data);
            imageData.data = nullptr;
        }
    }
}


// Handle Watch list
void SaveWatchList() {
    if (current_user.empty()) return;
    std::string exePath = GetExecutablePath();
    std::string userDirPath = exePath + "/" + USER_DIRECTORY;
    fs::path user_file = fs::path(userDirPath) / (current_user + ".txt");
    std::ofstream file(user_file);
    if (file.is_open()) {
        for (const auto& movie : watch_list) {
            file << movie.title << "|" << movie.release_year << "\n";
        }
        file.close();
    }
}
void AddToWatchList(const Movie& movie) {
    if (watch_list_titles.find(movie.title) == watch_list_titles.end()) {
        watch_list.push_back(movie);
        watch_list_titles.insert(movie.title);
        if (!current_user.empty()) {
            SaveWatchList();
        }
    }
}
bool RemoveFromWatchList(const std::string& title) {
    auto it = std::remove_if(watch_list.begin(), watch_list.end(),
        [&title](const Movie& movie) { return movie.title == title; });
    if (it != watch_list.end()) {
        watch_list.erase(it, watch_list.end());
        watch_list_titles.erase(title);
        if (!current_user.empty()) {
            SaveWatchList();
        }
        return true;
    }
    return false;
}
bool IsInWatchList(const std::string& title) {
    return watch_list_titles.find(title) != watch_list_titles.end();
}
void LoadWatchList(const std::string& username) {
    watch_list.clear();
    watch_list_titles.clear();
    std::string exePath = GetExecutablePath();
    std::string userDirPath = exePath + "/" + USER_DIRECTORY;
    fs::path user_file = fs::path(userDirPath) / (username + ".txt");
    std::ifstream file(user_file);
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            size_t delimiter_pos = line.find('|');
            if (delimiter_pos != std::string::npos) {
                Movie movie;
                movie.title = line.substr(0, delimiter_pos);
                movie.release_year = line.substr(delimiter_pos + 1);
                watch_list.push_back(movie);
                watch_list_titles.insert(movie.title);
            }
        }
        file.close();
    }
}

// User interface
bool UserLogin(const std::string& username) {
    std::string exePath = GetExecutablePath();
    std::string userDirPath = exePath + "/" + USER_DIRECTORY;

    if (!fs::exists(userDirPath)) {
        fs::create_directory(userDirPath);
    }

    fs::path user_file = fs::path(userDirPath) / (username + ".txt");
    if (fs::exists(user_file)) {
        // User exists, load their watch list
        current_user = username;
        LoadWatchList(username);
        return true;
    }
    else {
        // New user, create file
        std::ofstream file(user_file);
        if (file.is_open()) {
            file.close();
            current_user = username;
            watch_list.clear();
            watch_list_titles.clear();
            return true;
        }
    }
    return false;
}
void Logout() {
    current_user = "";
    watch_list.clear();
    watch_list_titles.clear();
    first_run = true;  // Return to home screen
    selected_movie = Movie();  // Clear selected movie
    selected_movie_index = -1;
}

// Handle font 
void LoadFonts(ImGuiIO& io) {
    std::string exePath = GetExecutablePath();
    std::string regularFontPath = exePath + "/" + REGULAR_FONT;
    std::string specialFontPath = exePath + "/" + SPECIAL_FONT;

    ImFont* regularFont = io.Fonts->AddFontFromFileTTF(regularFontPath.c_str(), FONT_SIZE);
   
    ImFont* specialFont45 = io.Fonts->AddFontFromFileTTF(specialFontPath.c_str(), 45.0f);
    
    ImFont* specialFont60 = io.Fonts->AddFontFromFileTTF(specialFontPath.c_str(), 60.0f);

    if (regularFont == nullptr || specialFont45 == nullptr || specialFont60 == nullptr) {
        std::cerr << "Failed to load fonts. Check file paths." << std::endl;
        std::cerr << "Regular font path: " << regularFontPath << std::endl;
        std::cerr << "Special font path: " << specialFontPath << std::endl;
    }

    io.Fonts->Build();
}

// Home screen 
void ResetApplication() {
    first_run = true;
    movie_list.clear();
    selected_movie = Movie();
    image_url.clear();
    movie_not_found = false;
    connection_error = false;
    selected_movie_index = -1;
    search_in_progress.store(false);
    movie_queue.clear();
    memset(title_input, 0, sizeof(title_input));
    memset(year_input, 0, sizeof(year_input));
    show_not_in_list_message = false;

    // Clear the image queue
    std::queue<std::string> empty;
    std::swap(image_queue, empty);

}

int main() {
    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return -1;
    }

    // Create a GLFW window
    window = glfwCreateWindow(1280, 720, "Movie Info", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);

    // Initialize GLAD
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to initialize GLAD" << std::endl;
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.WantCaptureMouse = true;
    ImGui::StyleColorsDark();
    LoadFonts(io);
    if (io.Fonts->Fonts.Size == 0) {
        std::cerr << "No fonts loaded. Aborting." << std::endl;
        return -1;
    }

    std::string welcomeImagePath = GetExecutablePath() + "/images/AGM.jpg";
    GLuint welcome_texture = LoadWelcomeImage(welcomeImagePath.c_str());
    if (welcome_texture == 0) {
        std::cerr << "Failed to load welcome image from: " << welcomeImagePath << std::endl;
    }

    // Setup Platform/Renderer backends
    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
        std::cerr << "Failed to initialize ImGui GLFW binding" << std::endl;
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    if (!ImGui_ImplOpenGL3_Init("#version 130")) {
        std::cerr << "Failed to initialize ImGui OpenGL3 binding" << std::endl;
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // Start the image loading thread
    std::thread image_thread(ImageLoadingThread);

    // Variables for ImGui input
    std::thread fetcher_thread;
    std::string message;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Get window size
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Create main ImGui window
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(display_w, display_h));
        ImGui::Begin("Movie Information", nullptr,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);

        // AGM button in the top left corner
        ImGui::SetCursorPos(ImVec2(10.0f, 10.0f));
        if (ImGui::Button("AGM")) {
            ResetApplication();
        }

        // User Profile button (move to top right corner)
        ImGui::SetCursorPos(ImVec2(display_w - 145.0f, 40.0f));
        if (ImGui::Button(current_user.empty() ? "Login" : "User Profile")) {
            ImGui::OpenPopup("UserProfilePopup");
        }

        ImFont* specialFont36 = io.Fonts->Fonts[1];
        ImFont* specialFont48 = io.Fonts->Fonts[2];

        // Display "Hello username" at the top of the screen in the middle, above the table lines
        if (!current_user.empty()) {
            ImGui::PushFont(specialFont36);
            ImGui::SetCursorPos(ImVec2(0, 27));
            ImGui::BeginGroup();
            ImGui::Dummy(ImVec2(display_w, 0.0f));
            float textWidth = ImGui::CalcTextSize(("Hello " + current_user).c_str()).x;
            ImGui::SetCursorPosX((display_w - textWidth) / 2.0f);
            ImGui::TextColored(ImVec4(0.0f, 0.4f, 1.0f, 1.0f), "Hello %s", current_user.c_str());
            ImGui::EndGroup();
            ImGui::PopFont();
        }

        // User Profile popup
        if (ImGui::BeginPopup("UserProfilePopup")) {
            static char username[256] = "";
            if (current_user.empty()) {
                ImGui::InputText("Username", username, IM_ARRAYSIZE(username));
                if (ImGui::Button("Login") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                    if (UserLogin(username)) {
                        ImGui::CloseCurrentPopup();
                    }
                    else {
                        // Handle login failure
                        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Login failed. Please try again.");
                    }
                }
            }
            else {
                ImGui::Text("Logged in as: %s", current_user.c_str());
                if (ImGui::Button("Logout")) {
                    Logout();
                    memset(username, 0, sizeof(username)); // Clear the username field
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }

        // Login Required popup
        if (ImGui::BeginPopup("LoginRequiredPopup")) {
            ImGui::Text("You need to log in to add movies to your watch list.");
            static char login_username[256] = "";
            ImGui::InputText("Username", login_username, IM_ARRAYSIZE(login_username));
            if (ImGui::Button("Login") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                if (UserLogin(login_username)) {
                    AddToWatchList(selected_movie);
                    ImGui::CloseCurrentPopup();
                }
                else {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Login failed. Please try again.");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Continue without login")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Set up two columns for movie details and search
        float column_width = display_w * 0.5f - 10; // 50% width for each column, with some padding
        ImGui::SetCursorPos(ImVec2(0, 80));
        ImGui::Columns(2, "MovieColumns", false);
        ImGui::SetColumnWidth(0, column_width);
        ImGui::SetColumnWidth(1, column_width);

        // Left column: Movie details or Welcome screen
        ImGui::BeginChild("LeftColumn", ImVec2(column_width, display_h - 100), true);
        if (first_run) {
            ImGui::SetCursorPosY(40);  // Add some top padding
            ImGui::PushFont(specialFont48);

            const char* welcome_lines[] = { "Welcome", "To", "AGM" };
            for (const char* line : welcome_lines) {
                float text_width = ImGui::CalcTextSize(line).x;
                ImGui::SetCursorPosX((column_width - text_width) / 2);
                ImGui::TextColored(ImVec4(0.0f, 0.4f, 1.0f, 1.0f), "%s", line);
            }

            ImGui::PopFont();

            // Image below the text
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 20);  // Add some space between text and image

            // Calculate the aspect ratio of the image
            float aspect_ratio = 1.0f;  // Assuming the image is square
            float image_width = column_width * 0.5f;  // Use 80% of the column width
            float image_height = image_width / aspect_ratio;

            ImGui::SetCursorPosX((column_width - image_width) / 2);  // Center the image
            ImGui::Image((void*)(intptr_t)welcome_texture, ImVec2(image_width, image_height));

            // Add the new text below the image
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 15);  // Add some space between image and new text
            ImGui::PushFont(specialFont48);  // Use the same special font as "Welcome to AGM"
            const char* greeting_text = "Greetings, what movie";
            const char* greeting_text2 = "would you like to see today?";
            float greeting_width = ImGui::CalcTextSize(greeting_text).x;
            float greeting_width2 = ImGui::CalcTextSize(greeting_text2).x;
            ImGui::SetCursorPosX((column_width - greeting_width) / 2);  // Center the text
            ImGui::TextColored(ImVec4(0.0f, 0.4f, 1.0f, 1.0f), "%s", greeting_text);
            ImGui::SetCursorPosX((column_width - greeting_width2) / 2);  // Center the second line
            ImGui::TextColored(ImVec4(0.0f, 0.4f, 1.0f, 1.0f), "%s", greeting_text2);
            ImGui::PopFont();
        }

        else if (selected_movie_index != -1 && !selected_movie.title.empty()) {
            if (fetch_in_progress.load()) {
                ImGui::Text("Fetching movie details...");
            }
            else {
                // Display movie details
                ImGui::Text("Title: %s", selected_movie.title.c_str());
                ImGui::Text("Year: %s", selected_movie.release_year.c_str());
                ImGui::Text("Director: %s", selected_movie.producer.c_str());
                ImGui::Text("Runtime: %s", selected_movie.runtime.c_str());
                ImGui::Text("IMDb Rating: %s", selected_movie.rating.c_str());
                ImGui::Text("Votes: %s", selected_movie.votes.c_str());

                if (!selected_movie.genres.empty()) {
                    ImGui::Text("Genres:");
                    for (const auto& genre : selected_movie.genres) {
                        ImGui::BulletText("%s", genre.c_str());
                    }
                }

                if (!selected_movie.cast.empty()) {
                    ImGui::Text("Cast:");
                    for (const auto& actor : selected_movie.cast) {
                        ImGui::BulletText("%s", actor.c_str());
                    }
                }
            }

            // Display movie poster
            if (!image_url.empty() && textureMap.find(image_url) != textureMap.end()) {
                const auto& imageData = textureMap[image_url];
                if (imageData.texture_id != 0) {
                    ImGui::Image((void*)(intptr_t)imageData.texture_id, ImVec2(200, 300));
                }
                else if (imageData.data != nullptr) {
                    // Create texture on the main thread
                    CreateTexture(image_url);
                    ImGui::Text("Image loaded, creating texture...");
                }
                else {
                    ImGui::Text("Image is still loading...");
                }
            }
            else {
                ImGui::Text("Image not available");
            }


            // Add to watch list button
            if (ImGui::Button("Add to Watch List")) {
                first_run = false;
                if (current_user.empty()) {
                    ImGui::OpenPopup("LoginRequiredPopup");
                }
                else {
                    AddToWatchList(selected_movie);
                    show_not_in_list_message = false;
                }
            }

            // Add this popup handling code
            if (ImGui::BeginPopup("LoginRequiredPopup")) {
                ImGui::Text("You need to log in to add movies to your watch list.");
                static char username[256] = "";
                ImGui::InputText("Username", username, IM_ARRAYSIZE(username));
                if (ImGui::Button("Login") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                    if (UserLogin(username)) {
                        AddToWatchList(selected_movie);
                        ImGui::CloseCurrentPopup();
                    }
                    else {
                        // Handle login failure
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Continue without login")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::SameLine();

            // Remove from watch list button
            if (ImGui::Button("Remove from Watch List")) {
                bool removed = RemoveFromWatchList(selected_movie.title);
                show_not_in_list_message = !removed;
            }

            // Messages for watch list status
            ImGui::BeginGroup();
            bool in_watch_list = IsInWatchList(selected_movie.title);
            if (in_watch_list) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Movie is in watch list");
            }
            else if (show_not_in_list_message) {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "The movie is not on watch list");
            }
            else {
                ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight())); // This ensures consistent vertical spacing
            }
            ImGui::EndGroup();
        }
        else {
            ImGui::Text("Select a movie to see details");
        }
        ImGui::EndChild();

        ImGui::NextColumn();

        // Right column: Search and movie list
        ImGui::BeginChild("SearchAndList", ImVec2(column_width, display_h - 100.0f), true);

        // Title search
        ImGui::Text("Search by Title:");
        bool triggerSearch = ImGui::InputText("Title", title_input, IM_ARRAYSIZE(title_input),
            ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::Text("Year (optional):");
        triggerSearch |= ImGui::InputText("Year", year_input, IM_ARRAYSIZE(year_input),
            ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::SameLine();
        if (ImGui::Button("Search") || triggerSearch) {
            if (fetcher_thread.joinable()) {
                fetcher_thread.join();
            }
            movie_list.clear();
            selected_movie = Movie();
            image_url.clear();
            movie_not_found = false;
            connection_error = false;
            selected_movie_index = -1;
            search_in_progress.store(true);
            movie_queue.clear();

            // Trigger fetching movie list based on title and use year as a filter
            fetcher_thread = std::thread([&]() {
                FetchMovieList(title_input, year_input);
                });
        }

        // Process movies from the queue
        if (search_in_progress.load()) {
            Movie movie;
            while (movie_queue.pop(movie)) {
                movie_list.push_back(movie);
            }
            if (movie_queue.is_finished()) {
                search_in_progress.store(false);
                if (!movie_list.empty()) {
                    first_run = false;
                }
                if (movie_list.empty()) {
                    movie_not_found = true;
                }
                else if (movie_list.size() == 1) {
                    // Automatically select and display the movie if it's the only one in the list
                    selected_movie_index = 0;
                    selected_movie = movie_list[0];
                    image_url.clear();

                    // Fetch detailed movie info
                    bool fetch_success = FetchMovieInfo(selected_movie);
                    if (fetch_success) {
                        // Update the movie in the list with the fetched details
                        movie_list[selected_movie_index] = selected_movie;

                        // Load the image if it's not already loaded
                        if (!image_url.empty()) {
                            std::unique_lock<std::mutex> lock(mtx);
                            if (textureMap.find(image_url) == textureMap.end()) {
                                image_queue.push(image_url);
                                cv.notify_one();
                            }
                        }
                    }
                    else {
                        std::cerr << "Failed to fetch movie details. Please try again." << std::endl;
                    }
                }
            }
        }
        // Display search results or messages
        if (search_in_progress.load()) {
            ImGui::Text("Searching...");
        }
        else if (!movie_list.empty()) {
            if (movie_list.size() > 1) {
                ImGui::Text("Select a movie:");
                // Create a child window for the scrollable list
                ImGui::BeginChild("MovieList", ImVec2(0, display_h * 0.3f), true);
                for (int i = 0; i < movie_list.size(); ++i) {
                    if (ImGui::Selectable(movie_list[i].title.c_str(), selected_movie_index == i)) {
                        if (fetch_thread.joinable()) {
                            fetch_thread.join(); // Ensure any previous fetch has completed
                        }
                        first_run = false;
                        selected_movie_index = i;
                        selected_movie = movie_list[i];
                        image_url.clear();
                        show_not_in_list_message = false;
                        fetch_in_progress.store(true);

                        fetch_thread = std::thread(FetchMovieDetails, i);
                    }
                }

                ImGui::EndChild();
            }
            else {
                // If there's only one movie, it's already selected and displayed
                ImGui::Text("Displaying the only movie found:");
                ImGui::Text("%s (%s)", selected_movie.title.c_str(), selected_movie.release_year.c_str());
            }
        }
        else if (movie_not_found) {
            ImGui::Text("No movies found. Please try another search.");
        }
        else if (connection_error) {
            ImGui::Text("Connection error occurred. Please check your internet connection and try again.");
        }

        // Button to show watch list
        ImGui::SetCursorPos(ImVec2(10, ImGui::GetCursorPosY()));
        if (ImGui::Button("To Watch List")) {
            ImGui::OpenPopup("WatchListPopup");
        }
        // Watch list popup
        if (ImGui::BeginPopup("WatchListPopup")) {
            ImGui::SetNextWindowSize(ImVec2(display_w * 0.8f, display_h * 0.8f));
            if (watch_list.empty()) {
                ImGui::Text("Watch list is empty.");
            }
            else {
                if (ImGui::BeginTable("WatchListTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Title");
                    ImGui::TableSetupColumn("Year");
                    ImGui::TableHeadersRow();

                    for (int i = 0; i < watch_list.size(); ++i) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        if (ImGui::Selectable(watch_list[i].title.c_str(), false,
                            ImGuiSelectableFlags_SpanAllColumns)) {
                            first_run = false;  // Exit the home screen only when a movie is selected
                            selected_movie_index = i;
                            selected_movie = watch_list[i];
                            image_url = selected_movie.poster_url;

                            // Fetch detailed movie info when selected
                            bool fetch_success = FetchMovieInfo(selected_movie);
                            if (fetch_success) {
                                // Load the image if it's not already loaded
                                if (!image_url.empty()) {
                                    std::unique_lock<std::mutex> lock(mtx);
                                    if (textureMap.find(image_url) == textureMap.end()) {
                                        image_queue.push(image_url);
                                        cv.notify_one();
                                    }
                                }
                                ImGui::CloseCurrentPopup();  // Close the popup after selection
                            }
                            else {
                                ImGui::Text("Failed to fetch movie details. Please try again.");
                            }
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%s", watch_list[i].release_year.c_str());
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndPopup();
        }

        ImGui::EndChild();
        ImGui::Columns(1);

        ImGui::End();

        // Rendering
        ImGui::Render();
        //        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    glfwSetWindowSizeCallback(window, [](GLFWwindow* window, int width, int height) {
        glViewport(0, 0, width, height);
        });

    // Cleanup
    image_thread_running = false;  // Signal the image loading thread to stop
    cv.notify_all();  // Wake up the image loading thread if it's waiting
    if (image_thread.joinable()) {
        image_thread.join(); 
    }

    if (fetcher_thread.joinable()) {
        fetcher_thread.join();
    }
    if (fetch_thread.joinable()) {
        fetch_thread.join();
    }

    // Clear any remaining items in the queue
    movie_queue.clear();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    glDeleteTextures(1, &welcome_texture);

    return 0;
}


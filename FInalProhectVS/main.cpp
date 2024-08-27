#define STB_IMAGE_IMPLEMENTATION
#define CPPHTTPLIB_OPENSSL_SUPPORT


#include <iostream>
#include <string>

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread_safe_queue.h>

#include <queue>
#include <map>
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
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

#define REGULAR_FONT "include/ImGui/misc/fonts/Karla-Regular.ttf"
#define SPECIAL_FONT "include/ImGui/misc/fonts/Pacifico-Regular.ttf"

#define USER_DIRECTORY "./users/"
#define FONT_SIZE 24.0f

struct Movie {
    std::string id;
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
    bool in_watch_list = false;
};

enum class ImageState {
    NotLoaded,
    Loading,
    Loaded,
    Error
};

struct ImageData {
    unsigned char* data = nullptr;
    int width = 0;
    int height = 0;
    int channels = 0;
    GLuint texture_id = 0;
    ImageState state = ImageState::NotLoaded;
};

// Global variables of the project:

// threads
std::mutex mtx;
std::condition_variable cv;
ThreadSafeQueue<Movie> movie_queue;
std::atomic<bool> image_thread_running(true);
std::atomic<bool> search_in_progress(false);
std::atomic<bool> fetch_in_progress(false);

// movie
std::queue<std::string> image_queue;
std::map<std::string, ImageData> textureMap;
std::string image_url;

std::vector<Movie> watch_list;
std::set<std::string> watch_list_titles;
bool movie_not_found = false;
Movie selected_movie;
int selected_movie_index = -1;
std::vector<Movie> movie_list;
bool show_not_in_list_message = false;
enum class SelectedList { None, SearchResults, WatchList };
SelectedList current_selected_list = SelectedList::None;
bool sort_watch_list_by_year = false;
bool sort_watch_list_ascending = true;
bool sort_movie_list_by_year = false;
bool sort_movie_list_ascending = true;

// user and window 
GLFWwindow* window;
std::string current_user;
bool first_run = true;
char title_input[256] = "";
char year_input[5] = "";
bool connection_error = false;
std::string api_key;

// Functions:

// General
std::string GetExecutablePath() {
    return fs::current_path().string();
}
void logError(const std::string& message) {
    std::ofstream logFile("error_log.txt", std::ios_base::app);
    if (logFile.is_open()) {
        time_t now = time(0);
        char* dt = ctime(&now);
        logFile << dt << ": " << message << std::endl;
        logFile.close();
    }
}
int FilterNumericInput(ImGuiInputTextCallbackData* data)
{
    if (data->EventChar < '0' || data->EventChar > '9')
        return 1;
    return 0;
}
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
    std::queue<std::string> empty;
    std::swap(image_queue, empty);
}

// Movie
bool IsInWatchList(const std::string& id) {
    return watch_list_titles.find(id) != watch_list_titles.end();
}
void FetchMovieList(const std::string& title, const std::string& year) {
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
                movie.id = item.value("imdbID", "");
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
            movie_not_found = true;
        }
    }
    else {
        connection_error = true;
    }

    movie_queue.setFinished();
}
bool FetchMovieInfo(Movie& movie) { // info of a spesific movie 
    try {
        std::string encoded_title = httplib::detail::encode_url(movie.title);
        std::string url = "/?t=" + encoded_title + "&y=" + movie.release_year + "&apikey=" + api_key;

        httplib::Client cli("https://www.omdbapi.com");
        auto res = cli.Get(url);

        if (!res) {
            logError("Connection error in FetchMovieInfo for movie: " + movie.title);
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
                movie.rating = response.value("imdbRating", "N/A");
                movie.votes = response.value("imdbVotes", "N/A");
                movie.id = response.value("imdbID", "");

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
            else {
                logError("API returned false response for movie: " + movie.title);
            }
        }
        else {
            logError("API returned non-200 status for movie: " + movie.title + ". Status: " + std::to_string(res->status));
        }
    }
    catch (const std::exception& e) {
        logError("Exception in FetchMovieInfo for movie: " + movie.title + ". Error: " + e.what());
    }

    connection_error = false;
    return false;
}
void FetchMovieInfoThread(const Movie& movie, int index) { // when removing a movie from watch list, fetches the next one
    Movie temp_movie = movie;
    bool fetch_success = FetchMovieInfo(temp_movie);
    if (fetch_success) {
        std::lock_guard<std::mutex> lock(mtx);
        selected_movie = temp_movie;
        if (index >= 0 && index < watch_list.size()) {
            watch_list[index] = temp_movie;
        }
        // Load the image if it's not already loaded
        if (!temp_movie.poster_url.empty()) {
            if (textureMap.find(temp_movie.poster_url) == textureMap.end()) {
                image_queue.push(temp_movie.poster_url);
                cv.notify_one();
            }
        }
    }
    else {
        logError("Failed to fetch movie info for: " + temp_movie.title);
    }
    fetch_in_progress.store(false);
}
void FetchMovieDetails(int index) {
    Movie temp_movie = movie_list[index];
    bool fetch_success = FetchMovieInfo(temp_movie);
    if (fetch_success) {
        bool need_to_fetch_image = false;
        std::string url_to_fetch;

        {
            std::lock_guard<std::mutex> lock(mtx);

            temp_movie.in_watch_list = IsInWatchList(temp_movie.id);
            movie_list[index] = temp_movie;

            if (index == selected_movie_index) {
                selected_movie = temp_movie;

                if (!temp_movie.poster_url.empty()) {
                    image_url = temp_movie.poster_url;
                    if (textureMap.find(image_url) == textureMap.end()) {
                        need_to_fetch_image = true;
                        url_to_fetch = image_url;
                    }
                }
            }
        }

        if (need_to_fetch_image) {
            std::lock_guard<std::mutex> lock(mtx);
            image_queue.push(url_to_fetch);
            cv.notify_one();
        }
    }
    fetch_in_progress.store(false);
}

// Image
bool IsValidImageData(const ImageData& imageData, const std::string& url) {
    if (imageData.data == nullptr || imageData.width == 0 || imageData.height == 0) {
        std::cerr << "Invalid image data for " << url << std::endl;
        return false;
    }
    return true;
}
void CleanupOnError(ImageData& imageData) {
    if (imageData.texture_id != 0) {
        glDeleteTextures(1, &imageData.texture_id);
        imageData.texture_id = 0;
    }
    if (imageData.data != nullptr) {
        stbi_image_free(imageData.data);
        imageData.data = nullptr;
    }
    imageData.state = ImageState::Error;
}
void CreateTexture(const std::string& url) {
    try {
        std::lock_guard<std::mutex> lock(mtx);
        if (textureMap.find(url) != textureMap.end()) {
            ImageData& imageData = textureMap[url];

            if (imageData.data == nullptr || imageData.width == 0 || imageData.height == 0 || imageData.channels == 0) {
                std::cerr << "Invalid image data for " << url << std::endl;
                CleanupOnError(imageData);
                return;
            }

            if (!glfwGetCurrentContext()) {
                std::cerr << "No OpenGL context current for thread" << std::endl;
                return;
            }

            if (imageData.texture_id == 0 && imageData.data != nullptr) {
                if (!IsValidImageData(imageData, url)) {
                    CleanupOnError(imageData);
                    return;
                }
                GLint maxTextureSize;
                glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
                if (imageData.width > maxTextureSize || imageData.height > maxTextureSize) {
                    std::cerr << "Texture size exceeds maximum allowed size for " << url << std::endl;
                    CleanupOnError(imageData);
                    return;
                }
                glGenTextures(1, &imageData.texture_id);
                if (imageData.texture_id == 0) {
                    std::cerr << "Failed to generate texture for " << url << std::endl;
                    CleanupOnError(imageData);
                    return;
                }
                glBindTexture(GL_TEXTURE_2D, imageData.texture_id);
                GLenum error = glGetError();
                if (error != GL_NO_ERROR) {
                    std::cerr << "OpenGL error in glBindTexture: " << error << std::endl;
                    CleanupOnError(imageData);
                    return;
                }

                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                error = glGetError();
                if (error != GL_NO_ERROR) {
                    std::cerr << "OpenGL error in glTexParameteri (MIN_FILTER): " << error << std::endl;
                    CleanupOnError(imageData);
                    return;
                }

                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                error = glGetError();
                if (error != GL_NO_ERROR) {
                    std::cerr << "OpenGL error in glTexParameteri (MAG_FILTER): " << error << std::endl;
                    CleanupOnError(imageData);
                    return;
                }

                GLenum internalFormat, format;
                if (imageData.channels == 1) {
                    internalFormat = GL_RED;
                    format = GL_RED;
                }
                else if (imageData.channels == 3) {
                    internalFormat = GL_RGB;
                    format = GL_RGB;
                }
                else if (imageData.channels == 4) {
                    internalFormat = GL_RGBA;
                    format = GL_RGBA;
                }
                else {
                    std::cerr << "Unsupported number of channels: " << imageData.channels << " for " << url << std::endl;
                    CleanupOnError(imageData);
                    return;
                }
                glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, imageData.width, imageData.height, 0,
                    format, GL_UNSIGNED_BYTE, imageData.data);
                error = glGetError();
                if (error != GL_NO_ERROR) {
                    std::cerr << "OpenGL error in glTexImage2D: " << error << std::endl;
                    CleanupOnError(imageData);
                    return;
                }
                imageData.state = ImageState::Loaded;

                glBindTexture(GL_TEXTURE_2D, 0);
                stbi_image_free(imageData.data);
                imageData.data = nullptr;
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Exception in CreateTexture: " << e.what() << std::endl;
    }
    catch (...) {
        std::cerr << "Unknown exception in CreateTexture" << std::endl;
    }
}
void EnsureImageLoaded(const std::string& url) {
    if (url.empty()) return;

    std::lock_guard<std::mutex> lock(mtx);
    auto it = textureMap.find(url);
    if (it == textureMap.end() || it->second.state == ImageState::NotLoaded) {
        // Image not loaded, start loading
        textureMap[url] = { nullptr, 0, 0, 0, 0, ImageState::Loading };
        image_queue.push(url);
        cv.notify_one();
    }
}
void DisplayMoviePoster(const std::string& poster_url, float image_width, float image_height) {
    if (!poster_url.empty()) {
        EnsureImageLoaded(poster_url);
        auto it = textureMap.find(poster_url);
        if (it != textureMap.end()) {
            switch (it->second.state) {
            case ImageState::Loaded:
                if (it->second.texture_id != 0) {
                    ImGui::Image((void*)(intptr_t)it->second.texture_id, ImVec2(image_width, image_height));
                }
                else {
                    ImGui::Text("Texture not created yet");
                    CreateTexture(poster_url);
                }
                break;
            case ImageState::Loading:
                ImGui::Text("Loading image...");
                break;
            case ImageState::Error:
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Failed to load image");
                break;
            case ImageState::NotLoaded:
                ImGui::Text("Image not loaded");
                break;
            }
        }
        else {
            ImGui::Text("Image not found in texture map");
        }
    }
    else {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "No poster available for this movie");
    }
}
GLuint LoadWelcomeImage(const char* filename)
{
    int width, height, channels;
    unsigned char* data = stbi_load(filename, &width, &height, &channels, STBI_rgb_alpha);
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

    // Since we're using STBI_rgb_alpha, we always have 4 channels
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);
    return texture_id;
}
void LoadImageFromUrl(const std::string& url) {
    if (url.empty()) {
        std::cerr << "Empty URL provided to LoadImageFromUrl" << std::endl;
        return;
    }

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

        if (data == nullptr) {
            std::cerr << "Failed to load image from " << url << ": " << stbi_failure_reason() << std::endl;
            return;
        }



        std::unique_lock<std::mutex> lock(mtx);
        textureMap[url] = { data, width, height, channels, 0, ImageState::Loaded };
        glfwPostEmptyEvent();

    }
    else {
        std::cerr << "Failed to download image from URL: " << url << ". Status: " << (res ? res->status : 0) << std::endl;
        std::lock_guard<std::mutex> lock(mtx);
        textureMap[url] = { nullptr, 0, 0, 0, 0, ImageState::Error };
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

            if (!url.empty() && url != "N/A") {
                try {
                    LoadImageFromUrl(url);
                }
                catch (const std::exception& e) {
                    std::cerr << "Exception in LoadImageFromUrl: " << e.what() << std::endl;
                }
                catch (...) {
                    std::cerr << "Unknown exception in LoadImageFromUrl" << std::endl;
                }
            }
            else {
                std::cerr << "Empty URL in image queue" << std::endl;
            }
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
            file << movie.id << "|" << movie.title << "|" << movie.release_year << "\n";
        }
        file.close();
    }
}
void AddToWatchList(const Movie& movie) {
    if (watch_list_titles.find(movie.id) == watch_list_titles.end()) {
        Movie watch_list_movie = movie;
        watch_list_movie.in_watch_list = true;
        watch_list.push_back(watch_list_movie);
        watch_list_titles.insert(movie.id);
        if (!current_user.empty()) {
            SaveWatchList();
        }
        if (selected_movie.id == movie.id) {
            selected_movie.in_watch_list = true;
        }
    }
}
std::pair<bool, int> RemoveFromWatchList(const std::string& id) {
    if (!IsInWatchList(id)) {
        return { false, -1 };  // The movie is not in the watch list, so we can't remove it
    }

    auto it = std::find_if(watch_list.begin(), watch_list.end(),
        [&id](const Movie& movie) { return movie.id == id; });

    if (it != watch_list.end()) {
        std::size_t removed_index = static_cast<std::size_t>(std::distance(watch_list.begin(), it));
        watch_list.erase(it);
        watch_list_titles.erase(id);

        // Update the in_watch_list status for all movies in movie_list
        for (auto& movie : movie_list) {
            if (movie.id == id) {
                movie.in_watch_list = false;
            }
        }

        if (!current_user.empty()) {
            SaveWatchList();
        }

        // Determine the new selected index
        std::size_t new_index = removed_index;
        if (new_index >= watch_list.size()) {
            new_index = watch_list.size() - 1;
        }

        return { true, new_index };
    }
    return { false, -1 };
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
            std::istringstream iss(line);
            std::string id, title, year;
            if (std::getline(iss, id, '|') && std::getline(iss, title, '|') && std::getline(iss, year)) {
                Movie movie;
                movie.id = id;
                movie.title = title;
                movie.release_year = year;
                movie.in_watch_list = true;
                watch_list.push_back(movie);
                watch_list_titles.insert(id);
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
    ResetApplication();
}

// Sort Functions
bool compareMoviesByTitle(const Movie& a, const Movie& b, bool ascending) {
    return ascending ? (a.title < b.title) : (a.title > b.title);
}
bool compareMoviesByYear(const Movie& a, const Movie& b, bool ascending) {
    return ascending ? (a.release_year < b.release_year) : (a.release_year > b.release_year);
}
void sortWatchList() {
    std::sort(watch_list.begin(), watch_list.end(),
        [](const Movie& a, const Movie& b) {
            if (sort_watch_list_by_year) {
                return compareMoviesByYear(a, b, sort_watch_list_ascending);
            }
            else {
                return compareMoviesByTitle(a, b, sort_watch_list_ascending);
            }
        });
}
void sortMovieList() {
    std::sort(movie_list.begin(), movie_list.end(),
        [](const Movie& a, const Movie& b) {
            if (sort_movie_list_by_year) {
                return compareMoviesByYear(a, b, sort_movie_list_ascending);
            }
            else {
                return compareMoviesByTitle(a, b, sort_movie_list_ascending);
            }
        });
}

// handle api_key
void read_api_key() {
    std::ifstream file("api_key.txt");  // File in the same directory as the source
    if (file.is_open()) {
        std::getline(file, api_key);
        file.close();
    }

    else {
        std::cerr << "Unable to open api_key.txt" << std::endl;
    }
}

// Main
int main() {
    read_api_key();

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
    std::thread fetch_thread;// for single movie details
    std::thread fetcher_thread;// for movie list 
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
        if (ImGui::Button("AGM Home Screen")) {
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
            ImFont* specialFont48 = io.Fonts->Fonts[2]; 
            ImGui::PushFont(specialFont48);

            ImGui::SetCursorPos(ImVec2(0, 20)); 
            ImGui::BeginGroup();
            ImGui::Dummy(ImVec2(display_w, 0.0f));

            std::string greeting = "Hello " + current_user;
            float textWidth = ImGui::CalcTextSize(greeting.c_str()).x;
            ImGui::SetCursorPosX((display_w - textWidth) / 2.0f);

            // Added a subtle shadow effect for better visibility
            ImVec2 originalPos = ImGui::GetCursorPos();
            ImGui::SetCursorPos(ImVec2(originalPos.x + 2, originalPos.y + 2));
            ImGui::TextColored(ImVec4(0.0f, 0.0f, 0.0f, 0.5f), "%s", greeting.c_str());
            ImGui::SetCursorPos(originalPos);

            ImGui::TextColored(ImVec4(0.0f, 0.5f, 1.0f, 1.0f), "%s", greeting.c_str());

            ImGui::EndGroup();
            ImGui::PopFont();
        }

        // User Profile popup
        if (ImGui::BeginPopup("UserProfilePopup")) {
            static char username[256] = "";
            if (current_user.empty()) {
                ImGui::InputText("Username", username, IM_ARRAYSIZE(username));
                ImGui::BeginDisabled(strlen(username) == 0);

                if (ImGui::Button("Login") || (ImGui::IsKeyPressed(ImGuiKey_Enter) && strlen(username) > 0)) {
                    if (UserLogin(username)) {
                        ImGui::CloseCurrentPopup();
                    }
                    else {
                        // Handle login failure
                        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Login failed. Please try again.");
                    }
                }
                ImGui::EndDisabled();

                ImGui::SameLine();
                if (ImGui::Button("Continue without login")) {
                    ImGui::CloseCurrentPopup();
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

            ImGui::BeginDisabled(strlen(login_username) == 0);
            if (ImGui::Button("Login") || (ImGui::IsKeyPressed(ImGuiKey_Enter) && strlen(login_username) > 0)) {
                if (UserLogin(login_username)) {
                    AddToWatchList(selected_movie);
                    ImGui::CloseCurrentPopup();
                }
                else {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Login failed. Please try again.");
                }
            }
            ImGui::EndDisabled();

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

        // Display movie details
        if (selected_movie_index != -1) {
            ImGui::BeginChild("MovieDetailsLayout", ImVec2(0, -1), false, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);

            // Movie Information
            if (fetch_in_progress.load()) {
                ImGui::Text("Fetching movie details...");
            }
            else {
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
          
            ImGui::Spacing();
     
           // Movie Poster
            float image_width = 200;
            float image_height = 300;
            DisplayMoviePoster(selected_movie.poster_url, image_width, image_height);
            ImGui::Spacing();

            // Add to watch list button
            if (ImGui::Button("Add to Watch List")) {
                first_run = false;
                if (current_user.empty()) {
                    ImGui::OpenPopup("LoginRequiredPopup");
                }
                else {
                    if (!IsInWatchList(selected_movie.id)) {
                        AddToWatchList(selected_movie);
                        selected_movie.in_watch_list = true;
                        // Update the movie in movie_list if it exists there
                        auto it = std::find_if(movie_list.begin(), movie_list.end(),
                            [&](const Movie& m) { return m.id == selected_movie.id; });
                        if (it != movie_list.end()) {
                            it->in_watch_list = true;
                        }
                    }
                    show_not_in_list_message = false;
                }
            }

            ImGui::SameLine();

            if (ImGui::Button("Remove from Watch List")) {
                if (current_selected_list != SelectedList::None && selected_movie_index != -1 && IsInWatchList(selected_movie.id)) {
                    auto [removed, new_index] = RemoveFromWatchList(selected_movie.id);
                    if (removed) {
                        ImGui::OpenPopup("RemovedFromWatchList");

                        // If we're viewing the watch list, update the selection
                        if (current_selected_list == SelectedList::WatchList) {
                            if (watch_list.empty()) {
                                current_selected_list = SelectedList::None;
                                selected_movie_index = -1;
                                selected_movie = Movie();
                                image_url.clear();
                            }
                            else {
                                selected_movie_index = new_index;
                                selected_movie = watch_list[selected_movie_index];
                                image_url = selected_movie.poster_url;

                                // Fetch detailed movie info for the newly selected movie
                                fetch_in_progress.store(true);
                                if (fetch_thread.joinable()) {
                                    fetch_thread.join();
                                }
                                fetch_thread = std::thread(FetchMovieInfoThread, selected_movie, selected_movie_index);
                            }
                        }
                        else {
                            // If we're in the search results, just update the status
                            selected_movie.in_watch_list = false;
                        }
                    }
                    else {
                        ImGui::OpenPopup("RemoveFromWatchListFailed");
                    }
                }
                else if (!IsInWatchList(selected_movie.id)) {
                    ImGui::OpenPopup("MovieNotInWatchList");
                }
                else {
                    ImGui::OpenPopup("NoMovieSelected");
                }
            }

            // Popup for movie not in watch list
            if (ImGui::BeginPopup("MovieNotInWatchList")) {
                ImGui::Text("This movie is not in your watch list.");
                if (ImGui::Button("OK")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            // Popup for successful removal
            if (ImGui::BeginPopup("RemovedFromWatchList")) {
                ImGui::Text("Movie successfully removed from your watch list.");
                if (ImGui::Button("OK")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            // Popup for failed removal
            if (ImGui::BeginPopup("RemoveFromWatchListFailed")) {
                ImGui::Text("Failed to remove movie from watch list. It may not be in the list.");
                if (ImGui::Button("OK")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            // Popup for no movie selected
            if (ImGui::BeginPopup("NoMovieSelected")) {
                ImGui::Text("Please select a movie first.");
                if (ImGui::Button("OK")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            // Add this popup handling code
            if (ImGui::BeginPopup("LoginRequiredPopup")) {
                static char login_username[256] = "";
                static bool reset_username = true;

                if (reset_username) {
                    memset(login_username, 0, sizeof(login_username));
                    reset_username = false;
                }

                ImGui::Text("You need to log in to add movies to your watch list.");
                ImGui::InputText("Username", login_username, IM_ARRAYSIZE(login_username));

                ImGui::BeginDisabled(strlen(login_username) == 0);
                if (ImGui::Button("Login") || (ImGui::IsKeyPressed(ImGuiKey_Enter) && strlen(login_username) > 0)) {
                    if (UserLogin(login_username)) {
                        AddToWatchList(selected_movie);
                        reset_username = true;
                        ImGui::CloseCurrentPopup();
                    }
                    else {
                        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Login failed. Please try again.");
                    }
                }
                ImGui::EndDisabled();

                ImGui::SameLine();
                if (ImGui::Button("Continue without login")) {
                    reset_username = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            else {
                // Reset the username when the popup is not open
                static bool reset_username = true;
                reset_username = true;
            }


            // Messages for watch list status
            ImGui::BeginGroup();
            bool in_watch_list = IsInWatchList(selected_movie.id);       
            if (in_watch_list) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Movie is in watch list");
            }
            else {
                ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
            }
            ImGui::EndGroup();
            ImGui::EndChild(); // MovieDetailsLayout
        }

        ImGui::EndChild();
        ImGui::NextColumn();

        // Right column: Search and movie list
        ImGui::BeginChild("SearchAndList", ImVec2(column_width, display_h - 100.0f), true);

        ImGui::SetCursorPos(ImVec2(10, ImGui::GetCursorPosY() + 10));
        ImGui::PushFont(io.Fonts->Fonts[0]);
        ImGui::SetWindowFontScale(1.2f);
        ImGui::TextColored(ImVec4(0.7f, 0.3f, 0.7f, 1.0f), "Search for a Movie:");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();

        // Title search
        ImGui::Text("Title:");
        bool triggerSearch = ImGui::InputText("Title", title_input, IM_ARRAYSIZE(title_input),
            ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::Text("Year (optional):");
        triggerSearch |= ImGui::InputText("Year", year_input, IM_ARRAYSIZE(year_input),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter,
            FilterNumericInput);

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
            ImGui::Text("Search Results:");
            // Create a child window for the scrollable list
            ImGui::BeginChild("SearchResults", ImVec2(0, display_h * 0.3f), true);
            if (ImGui::BeginTable("SearchResultsTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 0.7f);
                ImGui::TableSetupColumn("Year", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 0.3f);
                ImGui::TableHeadersRow();

                if (ImGui::TableGetSortSpecs()->SpecsDirty) {
                    ImGuiTableSortSpecs* sorts_specs = ImGui::TableGetSortSpecs();
                    if (sorts_specs->Specs->ColumnIndex == 0) {
                        sort_movie_list_by_year = false;
                        sort_movie_list_ascending = sorts_specs->Specs->SortDirection == ImGuiSortDirection_Ascending;
                    }
                    else {
                        sort_movie_list_by_year = true;
                        sort_movie_list_ascending = sorts_specs->Specs->SortDirection == ImGuiSortDirection_Ascending;
                    }
                    sortMovieList();
                    sorts_specs->SpecsDirty = false;
                }

                for (int i = 0; i < movie_list.size(); ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    std::string selectable_label = movie_list[i].title + "##" + std::to_string(i);
                    if (ImGui::Selectable(selectable_label.c_str(),
                        current_selected_list == SelectedList::SearchResults && selected_movie_index == i,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                        try {
                            first_run = false;
                            selected_movie_index = i;
                            current_selected_list = SelectedList::SearchResults;
                            selected_movie = movie_list[i];
                            image_url = selected_movie.poster_url;
                            show_not_in_list_message = false;

                            // Fetch detailed movie info when selected
                            fetch_in_progress.store(true);
                            if (fetch_thread.joinable()) {
                                fetch_thread.join();
                            }
                            fetch_thread = std::thread([i]() {
                                try {
                                    Movie temp_movie = movie_list[i];
                                    bool fetch_success = FetchMovieInfo(temp_movie);
                                    if (fetch_success) {
                                        std::lock_guard<std::mutex> lock(mtx);
                                        movie_list[i] = temp_movie;
                                        if (selected_movie_index == i && current_selected_list == SelectedList::SearchResults) {
                                            selected_movie = temp_movie;
                                            selected_movie.in_watch_list = IsInWatchList(selected_movie.id);
                                            // Load the image if it's not already loaded
                                            if (!selected_movie.poster_url.empty()) {
                                                if (textureMap.find(selected_movie.poster_url) == textureMap.end()) {
                                                    image_queue.push(selected_movie.poster_url);
                                                    cv.notify_one();
                                                }
                                            }
                                        }
                                    }
                                    else {
                                        logError("Failed to fetch movie info for: " + temp_movie.title);
                                    }
                                }
                                catch (const std::exception& e) {
                                    logError("Exception in fetch thread: " + std::string(e.what()));
                                }
                                fetch_in_progress.store(false);
                                });
                        }
                        catch (const std::exception& e) {
                            logError("Exception in movie selection: " + std::string(e.what()));
                        }
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", movie_list[i].release_year.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();
        }
        else if (movie_not_found) {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "No movies found. Please try another search.");
        }
        else if (connection_error) {
            ImGui::Text("Connection error occurred. Please check your internet connection and try again.");
        }

        // Watch List Display
        ImGui::SetCursorPos(ImVec2(10, ImGui::GetCursorPosY() + 10));
        ImGui::PushFont(io.Fonts->Fonts[0]);
        ImGui::SetWindowFontScale(1.2f);
        ImGui::TextColored(ImVec4(0.7f, 0.3f, 0.7f, 1.0f), "My Watch List:");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();

        if (current_user.empty()) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "Log in to see your watch list");
        }
        else if (watch_list.empty()) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "No movies in your watch list");
        }
        else {
            // Create a child window for the scrollable watch list
            ImGui::BeginChild("WatchList", ImVec2(0, display_h * 0.3f), true);
            if (ImGui::BeginTable("WatchListTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 0.7f);
                ImGui::TableSetupColumn("Year", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 0.3f);
                ImGui::TableHeadersRow();

                if (ImGui::TableGetSortSpecs()->SpecsDirty) {
                    ImGuiTableSortSpecs* sorts_specs = ImGui::TableGetSortSpecs();
                    if (sorts_specs->Specs->ColumnIndex == 0) {
                        sort_watch_list_by_year = false;
                        sort_watch_list_ascending = sorts_specs->Specs->SortDirection == ImGuiSortDirection_Ascending;
                    }
                    else {
                        sort_watch_list_by_year = true;
                        sort_watch_list_ascending = sorts_specs->Specs->SortDirection == ImGuiSortDirection_Ascending;
                    }
                    sortWatchList();
                    sorts_specs->SpecsDirty = false;
                }

                for (int i = 0; i < watch_list.size(); ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    std::string selectable_label = watch_list[i].title + "##" + watch_list[i].id;
                    if (ImGui::Selectable(selectable_label.c_str(),
                        current_selected_list == SelectedList::WatchList && selected_movie_index == i,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                        first_run = false;
                        selected_movie_index = i;
                        current_selected_list = SelectedList::WatchList;
                        selected_movie = watch_list[i];
                        image_url = selected_movie.poster_url;
                        show_not_in_list_message = false;

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
            ImGui::EndChild();
        }

        ImGui::EndChild();
        ImGui::Columns(1);

        ImGui::End();

        // Rendering
        ImGui::Render();
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
## Description
This application is a graphical user interface (GUI) program that allows users to search for movie information, view details about selected movies, and maintain a personal watch list. It uses the OMDb API to fetch movie data and displays it in an interactive interface.

## Necessary
- after first download insert:
- PATH=$(ProjectDir)include\OpenSSL;%PATH%
- inside Configuration properties/Debugging/Environment.
- make sure that All Configurations is selected in Configuration as well as All Platforms in Platform.
- make an API key from the OMBD website.
- create a txt folder where the main.cpp is located named: "api_key.txt".
- move your key inside the api_key.txt file and save.

## Features
- Search for movies by title and optionally by year
- View detailed information about selected movies, including:
  - Title, year, director, runtime
  - IMDb rating and number of votes
  - Genres and cast
  - Movie poster (when available)
- Add movies to a personal watch list
- Remove movies from the watch list
- User login functionality to save personal watch lists
- Graphical user interface with two-column layout for easy navigation

## Dependencies
- GLFW
- Dear ImGui
- stb_image
- nlohmann/json
- cpp-httplib
- OpenSSL

## Usage
1. Launch the application
2. Log in or create a new user profile
3. Use the search bar to find movies by title (and optionally by year)
4. Select a movie from the search results to view detailed information
5. Add or remove movies from your watch list
6. View your watch list by clicking the "To Watch List" button

## Contributing
Contributions to improve the application are welcome. Please follow these steps:
1. Fork the repository
2. Create a new branch for your feature
3. Commit your changes
4. Push to the branch
5. Create a new Pull Request

## Acknowledgments
- This project uses the OMDb API for fetching movie information
- Thanks to the creators and maintainers of the libraries used in this project

## Contact
leviguy0@gmail.com
adiaharoni55@gmail.com

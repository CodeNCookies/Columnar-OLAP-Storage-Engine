#include <iostream>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <iomanip>
#include <string>

int main() {
    const int N = 1000000;
    std::srand(42);  // fixed seed for reproducibility

    std::vector<std::string> countries = {
        "United States", "United Kingdom", "Germany", "France", "Japan",
        "Canada", "Australia", "Brazil", "India", "China",
        "Mexico", "Spain", "Italy", "Netherlands", "Sweden",
        "Norway", "Denmark", "Finland", "Portugal", "Ireland",
        "Austria", "Belgium", "Switzerland", "South Korea"
    };

    std::vector<std::string> categories = {
        "Electronics", "Clothing", "Books", "Home & Garden", "Sports",
        "Toys", "Food", "Health", "Beauty", "Automotive",
        "Music", "Movies", "Software", "Office Supplies", "Pet Supplies",
        "Jewelry", "Shoes", "Tools", "Garden", "Baby",
        "Furniture", "Appliances", "Grocery", "Pharmacy", "Stationery",
        "Art", "Crafts", "Party Supplies", "Luggage", "Eyewear",
        "Watches", "Fitness", "Outdoors", "Camping", "Fishing",
        "Hunting", "Cycling", "Golf", "Tennis", "Soccer",
        "Basketball", "Baseball", "Hockey", "Skiing", "Snowboarding",
        "Surfing", "Diving", "Climbing", "Running", "Yoga",
        "Pilates", "Meditation", "Nutrition", "Supplements", "Vitamins",
        "Skincare", "Haircare", "Makeup", "Fragrance", "Bath",
        "Bedding", "Bathroom", "Kitchen", "Dining", "Lighting",
        "Flooring", "Paint", "Hardware", "Electrical", "Plumbing",
        "HVAC", "Safety", "Security", "Surveillance", "Networking",
        "Computers", "Tablets", "Phones", "Accessories", "Gaming",
        "Printers", "Scanners", "Cameras", "Audio"
    };

    // Header
    std::cout << "id,date,country,category,product_id,quantity,price\n";

    for (int i = 0; i < N; ++i) {
        int id = i + 1;
        int year = 2024;
        int month = 1 + (std::rand() % 12);
        int day = 1 + (std::rand() % 28);
        int date = year * 10000 + month * 100 + day;

        std::string country = countries[std::rand() % countries.size()];
        std::string category = categories[std::rand() % categories.size()];
        int product_id = 1 + (std::rand() % 10000);
        int quantity = 1 + (std::rand() % 10);
        double price = (std::rand() % 10000) / 100.0 + 0.99;

        std::cout << id << "," << date << "," << country << ","
                  << category << "," << product_id << "," << quantity << ","
                  << std::fixed << std::setprecision(2) << price << "\n";

        if ((i + 1) % 100000 == 0)
            std::cerr << "Generated " << (i + 1) << " rows...\n";
    }
    return 0;
}


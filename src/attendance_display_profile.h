//current project - ATTENDANCE DISPLAY MODULE

#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
// TFTDisplayManager.h is included below in the color section (also pulls in TFT_eSPI.h)

// ══════════════════════════════════════════════════════════════════════════════
// ATTENDANCE DISPLAY - Separate module for employee profile display
// Does NOT interfere with existing dashboard.h
// 
// Features:
//   • Handles encrypted JSON responses from server
//   • Displays employee information with profile
//   • Shows loading, success, and error screens
//   • Works alongside your existing dashboard
// ══════════════════════════════════════════════════════════════════════════════

// ── Display geometry ──────────────────────────────────────────────────────────
#define ATT_SCREEN_W    320
#define ATT_SCREEN_H    240

// ── Colors — mapped to TFTDisplayManager's initialized palette ────────────────
// Using TFTColors:: ensures these always match the dashboard and respect the
// color-inversion setting applied during TFTDisplayManager::init().
#include "TFTDisplayManager.h"

#define ATT_BG_DARK     TFTColors::BG_DARK
#define ATT_BG_LIGHT    TFTColors::BG_LIGHT
#define ATT_PRIMARY     TFTColors::ACCENT_TEAL
#define ATT_SUCCESS     TFTColors::SUCCESS
#define ATT_ERROR       TFTColors::ERROR
#define ATT_WARNING     TFTColors::WARNING
#define ATT_TEXT        TFTColors::TEXT_PRIMARY
#define ATT_TEXT_DIM    TFTColors::TEXT_DIM

// ══════════════════════════════════════════════════════════════════════════════
// EMPLOYEE DATA STRUCTURE (from decrypted JSON)
// ══════════════════════════════════════════════════════════════════════════════
struct AttendanceEmployeeData {
    String uid;
    String idNumber;
    String fullName;
    String firstName;
    String lastName;
    String position;
    String department;
    String email;
    String status;
    String employmentType;
    bool accessGranted;
    String authTime;
    bool hasData;
};

// ══════════════════════════════════════════════════════════════════════════════
// ATTENDANCE DISPLAY CLASS
// ══════════════════════════════════════════════════════════════════════════════
class AttendanceDisplay {
private:
    TFT_eSPI* tft;
    AttendanceEmployeeData currentEmployee;
    
    // ──────────────────────────────────────────────────────────────────────────
    // Helper: Draw centered text
    // ──────────────────────────────────────────────────────────────────────────
    void drawCenteredText(const String& text, int16_t y, uint8_t font, uint16_t fgColor) {
        tft->setTextColor(fgColor);
        tft->setTextDatum(MC_DATUM);
        tft->drawString(text, ATT_SCREEN_W / 2, y, font);
        tft->setTextDatum(TL_DATUM);
    }
    
    // ──────────────────────────────────────────────────────────────────────────
    // Helper: Truncate text to fit width
    // ──────────────────────────────────────────────────────────────────────────
    String truncateText(const String& text, int16_t maxWidth, uint8_t font) {
        int16_t textWidth = tft->textWidth(text, font);
        
        if (textWidth <= maxWidth) {
            return text;
        }
        
        String truncated = text;
        while (tft->textWidth(truncated + "...", font) > maxWidth && truncated.length() > 0) {
            truncated.remove(truncated.length() - 1);
        }
        
        return truncated + "...";
    }
    
    // ──────────────────────────────────────────────────────────────────────────
    // Helper: Draw profile placeholder with initials
    // ──────────────────────────────────────────────────────────────────────────
    void drawProfilePlaceholder(int16_t x, int16_t y, int16_t radius, const String& initials) {
        tft->fillCircle(x, y, radius, ATT_PRIMARY);
        tft->drawCircle(x, y, radius, ATT_TEXT);
        
        tft->setTextColor(ATT_TEXT);
        tft->setTextDatum(MC_DATUM);
        tft->drawString(initials, x, y, 4);
        tft->setTextDatum(TL_DATUM);
    }

public:
    // ──────────────────────────────────────────────────────────────────────────
    // Constructor
    // Accepts an explicit TFT pointer or nullptr to auto-resolve from
    // TFTDisplayManager (preferred — ensures the shared display instance is used).
    // ──────────────────────────────────────────────────────────────────────────
    AttendanceDisplay(TFT_eSPI* tftInstance = nullptr) {
        tft = (tftInstance != nullptr) ? tftInstance : TFTDisplayManager::getTFT();
        currentEmployee.hasData = false;
    }
    
    // ══════════════════════════════════════════════════════════════════════════
    // PARSE ENCRYPTED JSON RESPONSE
    // ══════════════════════════════════════════════════════════════════════════
    bool parseEncryptedResponse(const String& jsonResponse, AttendanceEmployeeData& employee) {
        Serial.println("\n[AttDisplay] 📦 Parsing JSON response...");
        
        // Parse JSON
        DynamicJsonDocument doc(4096); // Larger buffer for encrypted data
        DeserializationError error = deserializeJson(doc, jsonResponse);
        
        if (error) {
            Serial.print("[AttDisplay] ❌ JSON parse error: ");
            Serial.println(error.c_str());
            return false;
        }
        
        // ── Check if response is encrypted ────────────────────────────────────
        if (doc.containsKey("encrypted") && doc["encrypted"] == true) {
            Serial.println("[AttDisplay] 🔐 Encrypted response detected");
            
            if (!doc.containsKey("data")) {
                Serial.println("[AttDisplay] ❌ No 'data' field in encrypted response");
                return false;
            }
            
            String encryptedData = doc["data"].as<String>();
            Serial.println("[AttDisplay] ⚠️  WARNING: Encrypted data detected!");
            Serial.println("[AttDisplay] ⚠️  Server should send PLAIN JSON to ESP32");
            Serial.println("[AttDisplay] ⚠️  Encryption is for web browser only");
            Serial.println("[AttDisplay] Encrypted data length: " + String(encryptedData.length()));
            
            // For now, we can't decrypt on ESP32 - server must send plain JSON
            return false;
        }
        
        // ── Parse plain JSON response ─────────────────────────────────────────
        Serial.println("[AttDisplay] ✅ Plain JSON response");
        
        // Check for success flag
        if (doc.containsKey("success") && !doc["success"]) {
            Serial.println("[AttDisplay] ❌ Response indicates failure");
            if (doc.containsKey("error")) {
                Serial.print("[AttDisplay] Error: ");
                Serial.println(doc["error"].as<String>());
            }
            return false;
        }
        
        // Extract employee object
        JsonObject empObj;
        if (doc.containsKey("employee")) {
            empObj = doc["employee"];
        } else {
            // Response might be the employee object directly
            empObj = doc.as<JsonObject>();
        }
        
        // Fill employee structure
        employee.uid = empObj["uid"] | "";
        employee.idNumber = empObj["id_number"] | empObj["idNumber"] | "";
        employee.fullName = empObj["full_name"] | empObj["fullName"] | "";
        employee.firstName = empObj["first_name"] | empObj["firstName"] | "";
        employee.lastName = empObj["last_name"] | empObj["lastName"] | "";
        employee.position = empObj["position"] | "";
        employee.department = empObj["department"] | "";
        employee.email = empObj["email"] | "";
        employee.status = empObj["status"] | "";
        employee.employmentType = empObj["employment_type"] | empObj["employmentType"] | "";
        
        // Access granted flag
        employee.accessGranted = doc["access_granted"] | empObj["access_granted"] | false;
        employee.authTime = doc["auth_time"] | empObj["auth_time"] | "";
        
        employee.hasData = true;
        
        // Log parsed data
        Serial.println("[AttDisplay] ✅ Employee data parsed:");
        Serial.println("  UID: " + employee.uid);
        Serial.println("  Name: " + employee.fullName);
        Serial.println("  Position: " + employee.position);
        Serial.println("  Department: " + employee.department);
        Serial.println("  Access: " + String(employee.accessGranted ? "GRANTED" : "DENIED"));
        
        return employee.hasData;
    }
    
    // ══════════════════════════════════════════════════════════════════════════
    // DISPLAY SCREENS
    // ══════════════════════════════════════════════════════════════════════════
    
    // ──────────────────────────────────────────────────────────────────────────
    // Show loading screen
    // ──────────────────────────────────────────────────────────────────────────
    void showLoading() {
        tft->fillScreen(ATT_BG_DARK);
        
        // Title
        drawCenteredText("AUTHENTICATING", 70, 4, ATT_TEXT);
        
        // Animated dots
        static uint8_t dotCount = 0;
        String dots = "";
        for (uint8_t i = 0; i < 3; i++) {
            dots += (i < (dotCount % 3) + 1) ? "●" : "○";
            dots += " ";
        }
        dotCount++;
        
        drawCenteredText(dots, 110, 4, ATT_PRIMARY);
        
        // Message
        drawCenteredText("Please wait...", 150, 2, ATT_TEXT_DIM);
    }
    
    // ──────────────────────────────────────────────────────────────────────────
    // Show employee card (main display)
    // ──────────────────────────────────────────────────────────────────────────
    void showEmployeeCard(const AttendanceEmployeeData& employee) {
        tft->fillScreen(ATT_BG_DARK);
        
        // ── Header ────────────────────────────────────────────────────────────
        tft->fillRect(0, 0, ATT_SCREEN_W, 35, ATT_PRIMARY);
        drawCenteredText("ACCESS GRANTED", 17, 4, ATT_TEXT);
        
        // ── Profile Picture Placeholder ───────────────────────────────────────
        String initials = "";
        if (employee.firstName.length() > 0) initials += employee.firstName.charAt(0);
        if (employee.lastName.length() > 0) initials += employee.lastName.charAt(0);
        
        drawProfilePlaceholder(ATT_SCREEN_W / 2, 70, 30, initials);
        
        // ── Employee Name ─────────────────────────────────────────────────────
        String displayName = truncateText(employee.fullName, ATT_SCREEN_W - 40, 4);
        drawCenteredText(displayName, 115, 4, ATT_TEXT);
        
        // ── Position ──────────────────────────────────────────────────────────
        String position = truncateText(employee.position, ATT_SCREEN_W - 40, 2);
        tft->setTextColor(ATT_PRIMARY);
        tft->setTextDatum(MC_DATUM);
        tft->drawString(position, ATT_SCREEN_W / 2, 135, 2);
        
        // ── Info Grid ─────────────────────────────────────────────────────────
        int16_t infoY = 160;
        
        tft->setTextDatum(TL_DATUM);
        tft->setTextColor(ATT_TEXT_DIM);
        
        // Left column
        tft->drawString("ID:", 30, infoY, 2);
        tft->setTextColor(ATT_TEXT);
        tft->drawString(employee.idNumber, 30, infoY + 15, 2);
        
        // Right column
        tft->setTextColor(ATT_TEXT_DIM);
        tft->drawString("Dept:", 180, infoY, 2);
        tft->setTextColor(ATT_TEXT);
        String dept = truncateText(employee.department, 120, 2);
        tft->drawString(dept, 180, infoY + 15, 2);
        
        // ── Status ────────────────────────────────────────────────────────────
        uint16_t statusColor = (employee.status == "Active") ? ATT_SUCCESS : ATT_WARNING;
        tft->fillCircle(ATT_SCREEN_W - 80, ATT_SCREEN_H - 20, 5, statusColor);
        tft->setTextColor(ATT_TEXT_DIM);
        tft->drawString(employee.status, ATT_SCREEN_W - 70, ATT_SCREEN_H - 25, 2);
        
        tft->setTextDatum(TL_DATUM);
    }
    
    // ──────────────────────────────────────────────────────────────────────────
    // Show success screen
    // ──────────────────────────────────────────────────────────────────────────
    void showSuccess(const String& employeeName) {
        tft->fillScreen(ATT_BG_DARK);
        
        // Success checkmark
        tft->fillCircle(ATT_SCREEN_W / 2, 80, 30, ATT_SUCCESS);
        tft->setTextColor(ATT_TEXT);
        tft->setTextDatum(MC_DATUM);
        tft->drawString("✓", ATT_SCREEN_W / 2, 80, 7);
        
        // Title
        drawCenteredText("SUCCESS!", 140, 4, ATT_SUCCESS);
        
        // Message
        drawCenteredText("Attendance Recorded", 170, 2, ATT_TEXT);
        
        // Employee name
        String name = truncateText(employeeName, ATT_SCREEN_W - 60, 2);
        drawCenteredText(name, 195, 2, ATT_TEXT_DIM);
        
        tft->setTextDatum(TL_DATUM);
    }
    
    // ──────────────────────────────────────────────────────────────────────────
    // Show error screen
    // ──────────────────────────────────────────────────────────────────────────
    void showError(const String& errorMessage) {
        tft->fillScreen(ATT_BG_DARK);
        
        // Error X
        tft->fillCircle(ATT_SCREEN_W / 2, 80, 30, ATT_ERROR);
        tft->setTextColor(ATT_TEXT);
        tft->setTextDatum(MC_DATUM);
        tft->drawString("✖", ATT_SCREEN_W / 2, 80, 7);
        
        // Title
        drawCenteredText("ACCESS DENIED", 140, 4, ATT_ERROR);
        
        // Error message (word wrap)
        tft->setTextColor(ATT_TEXT_DIM);
        int16_t y = 170;
        int16_t maxWidth = ATT_SCREEN_W - 60;
        String remaining = errorMessage;
        
        while (remaining.length() > 0 && y < ATT_SCREEN_H - 30) {
            String line = remaining;
            
            // Find break point
            while (tft->textWidth(line, 2) > maxWidth && line.length() > 0) {
                int lastSpace = line.lastIndexOf(' ');
                if (lastSpace > 0) {
                    line = line.substring(0, lastSpace);
                } else {
                    line.remove(line.length() - 1);
                }
            }
            
            tft->drawString(line, ATT_SCREEN_W / 2, y, 2);
            y += 20;
            
            remaining = remaining.substring(line.length());
            remaining.trim();
        }
        
        tft->setTextDatum(TL_DATUM);
    }
    
    // ══════════════════════════════════════════════════════════════════════════
    // CONVENIENCE METHODS
    // ══════════════════════════════════════════════════════════════════════════
    
    // Get current employee data
    AttendanceEmployeeData getCurrentEmployee() {
        return currentEmployee;
    }
    
    // Set current employee data
    void setCurrentEmployee(const AttendanceEmployeeData& employee) {
        currentEmployee = employee;
    }
    
    // Check if employee data is available
    bool hasEmployeeData() {
        return currentEmployee.hasData;
    }
    
    // Clear employee data
    void clearEmployeeData() {
        currentEmployee.hasData = false;
        currentEmployee.uid = "";
        currentEmployee.fullName = "";
    }
};
import Foundation
import Combine
import CoreLocation

// MARK: - Location Manager
//
// Thin wrapper around CLLocationManager for geo-tagging uploads.
// Uses best accuracy since the user wants precise map placement.

class LocationManager: NSObject, ObservableObject, CLLocationManagerDelegate {
    static let shared = LocationManager()

    private let manager = CLLocationManager()

    @Published var location: CLLocation?
    @Published var authorizationStatus: CLAuthorizationStatus = .notDetermined

    override init() {
        super.init()
        manager.delegate = self
        manager.desiredAccuracy = kCLLocationAccuracyBest
        authorizationStatus = manager.authorizationStatus
    }

    /// Request "when in use" authorization if not already granted.
    func requestPermission() {
        if authorizationStatus == .notDetermined {
            manager.requestWhenInUseAuthorization()
        }
    }

    /// Request a single high-accuracy location fix.
    func requestLocation() {
        guard authorizationStatus == .authorizedWhenInUse
           || authorizationStatus == .authorizedAlways else {
            requestPermission()
            return
        }
        manager.requestLocation()
    }

    var isAuthorized: Bool {
        authorizationStatus == .authorizedWhenInUse
        || authorizationStatus == .authorizedAlways
    }

    // MARK: - CLLocationManagerDelegate

    func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        location = locations.last
    }

    func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        print("[LocationManager] Error: \(error.localizedDescription)")
    }

    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        authorizationStatus = manager.authorizationStatus
        if isAuthorized {
            manager.requestLocation()
        }
    }
}

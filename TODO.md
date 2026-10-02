# Quill Moon Clock — TODO

## Future Enhancements

### GNSS-derived observer location
- Read latitude and longitude from the BZ-181 GNSS module.
- Use GNSS coordinates for moonrise/moonset calculations.
- Retain configured coordinates as a fallback when no valid GNSS fix is available.
- Consider deriving the displayed location name from the GNSS position.
- Investigate using GNSS time as an alternative/reference time source.

### User-customisable colours
- Move display colours out of `main.cpp` into configuration.
- Allow configuration of:
  - Location text
  - Moonrise
  - Moonset
  - Clock
  - Date
  - Moon phase
- Retain sensible default colours.

## Possible Later Enhancements

- Display GNSS fix/status indicator.
- Automatic location update after significant movement.
- User-selectable 12/24-hour time format.
- User-selectable location display name.
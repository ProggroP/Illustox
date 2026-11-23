// Clay-Konfigurationsseite. Nutzersichtbare Texte englisch.
// messageKey muss Zeichen fuer Zeichen zu package.json passen.
module.exports = [
  {
    "type": "heading",
    "defaultValue": "Illustox"
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Landscape"
      },
      {
        "type": "select",
        "messageKey": "WORLD",
        "defaultValue": "0",
        "label": "Colour world",
        "options": [
          { "label": "Desert", "value": "0" },
          { "label": "Dusk", "value": "1" },
          { "label": "Sage", "value": "2" },
          { "label": "Ocean", "value": "3" },
          { "label": "Sunset", "value": "4" },
          { "label": "Candy", "value": "5" },
          { "label": "Surprise me (changes with every scene)", "value": "99" }
        ]
      },
      {
        "type": "select",
        "messageKey": "TEXTURE",
        "defaultValue": "0",
        "label": "Style",
        "options": [
          { "label": "Flat (pure palette colours)", "value": "0" },
          { "label": "Print (mixed pastel tones, soft sky)", "value": "1" }
        ]
      },
      {
        "type": "select",
        "messageKey": "INTERVAL",
        "defaultValue": "60",
        "label": "New landscape",
        "options": [
          { "label": "Every minute", "value": "1" },
          { "label": "Every 15 minutes", "value": "15" },
          { "label": "Every hour", "value": "60" }
        ]
      },
      {
        "type": "toggle",
        "messageKey": "SHAKE",
        "defaultValue": true,
        "label": "Shake for a new landscape"
      }
    ]
  },
  {
    "type": "submit",
    "defaultValue": "Save"
  }
];

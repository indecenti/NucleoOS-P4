// sheets_templates.h — Fogli's starting templates in the five UI languages. One string per
// template and language: rows separated by '\n', cells by '|'. Formulas are written in Excel's
// English notation (any language reads them); numbers use '.' only for decimals (two digits).
#pragma once

enum { TPL_BUDGET, TPL_SHOP, TPL_HOURS, TPL_COUNT_ };

static const char *const kTpl[TPL_COUNT_][5] = {
    {   // TPL_BUDGET
        "Item|Planned|Actual|Difference\nIncome\nSalary|1800|1800|=C3-B3\nOther income|150|90|=C4-B4\n"
        "Total income|=SUM(B3:B4)|=SUM(C3:C4)|=C5-B5\n\nExpenses\nRent|750|750|=B8-C8\nBills|180|196.40|=B9-C9\n"
        "Groceries|400|371.85|=B10-C10\nTransport|90|64|=B11-C11\nLeisure|120|142.50|=B12-C12\n"
        "Total expenses|=SUM(B8:B12)|=SUM(C8:C12)|=B13-C13\n\nBalance|=B5-B13|=C5-C13|=C15-B15",
        "Voce|Previsto|Effettivo|Differenza\nEntrate\nStipendio|1800|1800|=C3-B3\nAltre entrate|150|90|=C4-B4\n"
        "Totale entrate|=SUM(B3:B4)|=SUM(C3:C4)|=C5-B5\n\nUscite\nAffitto|750|750|=B8-C8\nBollette|180|196.40|=B9-C9\n"
        "Spesa|400|371.85|=B10-C10\nTrasporti|90|64|=B11-C11\nSvago|120|142.50|=B12-C12\n"
        "Totale uscite|=SUM(B8:B12)|=SUM(C8:C12)|=B13-C13\n\nSaldo|=B5-B13|=C5-C13|=C15-B15",
        "Concepto|Previsto|Real|Diferencia\nIngresos\nSueldo|1800|1800|=C3-B3\nOtros ingresos|150|90|=C4-B4\n"
        "Total ingresos|=SUM(B3:B4)|=SUM(C3:C4)|=C5-B5\n\nGastos\nAlquiler|750|750|=B8-C8\nFacturas|180|196.40|=B9-C9\n"
        "Compra|400|371.85|=B10-C10\nTransporte|90|64|=B11-C11\nOcio|120|142.50|=B12-C12\n"
        "Total gastos|=SUM(B8:B12)|=SUM(C8:C12)|=B13-C13\n\nSaldo|=B5-B13|=C5-C13|=C15-B15",
        "Poste|Prévu|Réel|Écart\nRevenus\nSalaire|1800|1800|=C3-B3\nAutres revenus|150|90|=C4-B4\n"
        "Total revenus|=SUM(B3:B4)|=SUM(C3:C4)|=C5-B5\n\nDépenses\nLoyer|750|750|=B8-C8\nFactures|180|196.40|=B9-C9\n"
        "Courses|400|371.85|=B10-C10\nTransports|90|64|=B11-C11\nLoisirs|120|142.50|=B12-C12\n"
        "Total dépenses|=SUM(B8:B12)|=SUM(C8:C12)|=B13-C13\n\nSolde|=B5-B13|=C5-C13|=C15-B15",
        "Posten|Geplant|Tatsächlich|Differenz\nEinnahmen\nGehalt|1800|1800|=C3-B3\nSonstige Einnahmen|150|90|=C4-B4\n"
        "Einnahmen gesamt|=SUM(B3:B4)|=SUM(C3:C4)|=C5-B5\n\nAusgaben\nMiete|750|750|=B8-C8\nRechnungen|180|196.40|=B9-C9\n"
        "Lebensmittel|400|371.85|=B10-C10\nVerkehr|90|64|=B11-C11\nFreizeit|120|142.50|=B12-C12\n"
        "Ausgaben gesamt|=SUM(B8:B12)|=SUM(C8:C12)|=B13-C13\n\nSaldo|=B5-B13|=C5-C13|=C15-B15",
    },
    {   // TPL_SHOP
        "Item|Quantity|Price|Total\nBread|2|1.80|=B2*C2\nMilk|3|1.25|=B3*C3\nFruit|1|4.90|=B4*C4\nPasta|4|0.95|=B5*C5\n"
        "Coffee|1|5.40|=B6*C6\n\nTotal|=SUM(B2:B6)||=SUM(D2:D6)",
        "Articolo|Quantità|Prezzo|Totale\nPane|2|1.80|=B2*C2\nLatte|3|1.25|=B3*C3\nFrutta|1|4.90|=B4*C4\nPasta|4|0.95|=B5*C5\n"
        "Caffè|1|5.40|=B6*C6\n\nTotale|=SUM(B2:B6)||=SUM(D2:D6)",
        "Artículo|Cantidad|Precio|Total\nPan|2|1.80|=B2*C2\nLeche|3|1.25|=B3*C3\nFruta|1|4.90|=B4*C4\nPasta|4|0.95|=B5*C5\n"
        "Café|1|5.40|=B6*C6\n\nTotal|=SUM(B2:B6)||=SUM(D2:D6)",
        "Article|Quantité|Prix|Total\nPain|2|1.80|=B2*C2\nLait|3|1.25|=B3*C3\nFruits|1|4.90|=B4*C4\nPâtes|4|0.95|=B5*C5\n"
        "Café|1|5.40|=B6*C6\n\nTotal|=SUM(B2:B6)||=SUM(D2:D6)",
        "Artikel|Menge|Preis|Summe\nBrot|2|1.80|=B2*C2\nMilch|3|1.25|=B3*C3\nObst|1|4.90|=B4*C4\nNudeln|4|0.95|=B5*C5\n"
        "Kaffee|1|5.40|=B6*C6\n\nGesamt|=SUM(B2:B6)||=SUM(D2:D6)",
    },
    {   // TPL_HOURS
        "Day|Hours|Rate|Pay\nMonday|8|15|=B2*C2\nTuesday|7.50|15|=B3*C3\nWednesday|8|15|=B4*C4\nThursday|6|15|=B5*C5\n"
        "Friday|8|15|=B6*C6\n\nTotal|=SUM(B2:B6)|=AVERAGE(C2:C6)|=SUM(D2:D6)",
        "Giorno|Ore|Tariffa|Compenso\nLunedì|8|15|=B2*C2\nMartedì|7.50|15|=B3*C3\nMercoledì|8|15|=B4*C4\nGiovedì|6|15|=B5*C5\n"
        "Venerdì|8|15|=B6*C6\n\nTotale|=SUM(B2:B6)|=AVERAGE(C2:C6)|=SUM(D2:D6)",
        "Día|Horas|Tarifa|Pago\nLunes|8|15|=B2*C2\nMartes|7.50|15|=B3*C3\nMiércoles|8|15|=B4*C4\nJueves|6|15|=B5*C5\n"
        "Viernes|8|15|=B6*C6\n\nTotal|=SUM(B2:B6)|=AVERAGE(C2:C6)|=SUM(D2:D6)",
        "Jour|Heures|Taux|Paie\nLundi|8|15|=B2*C2\nMardi|7.50|15|=B3*C3\nMercredi|8|15|=B4*C4\nJeudi|6|15|=B5*C5\n"
        "Vendredi|8|15|=B6*C6\n\nTotal|=SUM(B2:B6)|=AVERAGE(C2:C6)|=SUM(D2:D6)",
        "Tag|Stunden|Satz|Lohn\nMontag|8|15|=B2*C2\nDienstag|7.50|15|=B3*C3\nMittwoch|8|15|=B4*C4\nDonnerstag|6|15|=B5*C5\n"
        "Freitag|8|15|=B6*C6\n\nGesamt|=SUM(B2:B6)|=AVERAGE(C2:C6)|=SUM(D2:D6)",
    },
};

/* Don't mess with this bit unless 
   you know what you're doing */
typedef struct pellet {
  const char * pellet_name;
  const char * pellet_mfr;
  float pellet_weight_grains;
  float pellet_caliber_inch;
  float pellet_weight_grams;
  float pellet_caliber_mm;
} pellet_t;

#define NUM_PELLETS (sizeof(my_pellets)/sizeof(pellet_t))
/************************************************************************************/
/* Add your pellets here !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
/**************************************************************** ********************/
/* Every row EXCEPT the last one must have a comma at the end. 
   
   Format is:
   
Name, Manufacturere, weight in grains, calibre in inches, weight in grams, calibre in mm
*/
pellet_t my_pellets[] = {
    { "Meisterkugeln", "RWS", 8.2, 0.177, 0.53, 4.5 },
    { "Sport", "H&N", 8.64, 0.177, 0.56, 4.5 },
    { "Field Tgt Trophy", "H&N", 8.64, 0.177, 0.56, 4.5 },
    { "Exact Jumbo", "JSB", 15.89, 0.22, 1.03, 5.5 },
    { "Exact Heavy", "JSB", 18.13, 0.22, 1.175, 5.5 }
};
/************************************************************************************/

#include "vtkPNGReader.h"
#include "vtkImageFlip.h"
#include <sys/stat.h>

#ifdef VTK_USE_ANSI_STDLIB
#define VTK_IOS_NOCREATE 
#else
#define VTK_IOS_NOCREATE | ios::nocreate
#endif

int file_exists(const char *filename)
{
  struct stat fs;
  return (stat(filename, &fs) != 0) ? 0 : 1;
}

long int modified_time(const char *filename)
{
  struct stat fs;
  if (stat(filename, &fs) != 0) 
    {
    return 0;
    }
  else
    {
    return (long int)fs.st_mtime;
    }
}

const char* name(const char *filename)
{
  char *forward = strrchr(filename, '/') + 1;
  char *backward = strrchr(filename, '\\') + 1;
  return (forward > backward) ? forward : backward;
}

int main(int argc, char **argv)
{
  // Usage

  if (argc < 3)
    {
    cerr << "Usage: " << argv[0] << " header.h image.png [image.png image.png...] [UPDATE]" << endl;
    return 1;
    }

  // If in UPDATE mode, do something only if one of the images is newer
  // than the header file
  
  if (!strcmp(argv[argc - 1], "UPDATE"))
    {
    argc--;
    if (file_exists(argv[1]))
      {
      long int header_modified_time =  modified_time(argv[1]);
      int i = 2;
      while (i < argc && modified_time(argv[i]) <= header_modified_time)
        {
        i++;
        }
      if (i == argc)
        {
        cout << name(argv[1]) << " is up-to-date" << endl;
        return 0;
        }
      }
    }

  // Open header file

  ofstream out(argv[1], ios::out);
  if (out.fail())
    {
    cerr << "Cannot open: " << argv[2] << " for writing" << endl;
    return 3;
    }

  cout << "Creating " << name(argv[1]) << endl;

  // Loop over each image

  vtkPNGReader *pr = vtkPNGReader::New();
  vtkImageFlip *flip = vtkImageFlip::New();

  int i;
  for (i = 2; i < argc; i++)
    {
    
    // Check if image exists

    if (!file_exists(argv[i]))
      {
      cerr << "Cannot open: " << argv[2] << " for reading" << endl;
      return 2;
      }
  
    // Read as PNG

    char buffer[1024];
    strcpy(buffer, name(argv[i]));

    cout << "  - from: " << buffer << endl;


    pr->SetFileName(argv[i]);
    pr->Update();

    if (pr->GetOutput()->GetNumberOfScalarComponents() != 3 &&
        pr->GetOutput()->GetNumberOfScalarComponents() != 4)
      {
      cerr << "Can only convert RGB or RGBA images" << endl;
      pr->Delete();
      continue;
      }

    // Flip image (in VTK, [0,0] is lower left)

    flip->SetInput(pr->GetOutput());
    flip->SetFilteredAxis(1);
    flip->Update();

    // Output image info

    int *dim = flip->GetOutput()->GetDimensions();
    int width = dim[0];
    int height = dim[1];
    int pixel_size = flip->GetOutput()->GetNumberOfScalarComponents();
    unsigned long nb_of_pixels = width * height;

    out << "/* " << endl
        << " * This file is generated by ImageConvert from image:" << endl
        << " *    " << buffer << endl
        << " */" << endl;

    buffer[strlen(buffer) - 4] = 0;
  
    out << "#define image_" << buffer << "_width      " << width << endl
        << "#define image_" << buffer << "_height     " << height << endl
        << "#define image_" << buffer << "_pixel_size " << pixel_size << endl
        << endl
        << "static unsigned char image_" << buffer << "[] = {" << endl
        << "  ";

    // Loop over pixels

    unsigned char *image = 
      (unsigned char *)(flip->GetOutput()->GetScalarPointer());

    unsigned long row_size = width;
    unsigned long cc;

    for (cc = 0; cc < nb_of_pixels; cc++)
      {    
      
      // Output marker for each line

      if (cc % row_size == 0)
        {
        out << endl << "/* " << (cc / row_size) << " */ ";
        }
    
      // Output pixel

      if (pixel_size == 4 && (unsigned int)image[cc * pixel_size + 3] == 0)
        {
        out << "0, 0, 0, 0";
        }
      else
        {
        out << (unsigned int)image[cc * pixel_size] << ", "
            << (unsigned int)image[cc * pixel_size + 1] << ", "
            << (unsigned int)image[cc * pixel_size + 2];
        if (pixel_size == 4)
          {
          out << ", " << (unsigned int)image[cc * pixel_size + 3];
          }
        }

      // Pixel Separator

      if (cc != (nb_of_pixels)-1)
        {
        out << ", ";
        }

      // Line separator

      if (cc % pixel_size == 11)
        {
        out << endl << "  ";
        }
      }

    out << "};" << endl << endl;

    } // Next file

  out.close();

  pr->Delete();
  flip->Delete();

  return 0;
}

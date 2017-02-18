/* 
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

//-----------------------------------------------------------------------------
//
// optixProgressivePhotonMap: progressive photon mapping scene
//
//-----------------------------------------------------------------------------

#ifndef __APPLE__
#  include <GL/glew.h>
#  if defined( _WIN32 )
#    include <GL/wglew.h>
#  endif
#endif

#include <GLFW/glfw3.h>

#include <optixu/optixpp_namespace.h>
#include <optixu/optixu_aabb_namespace.h>
#include <optixu/optixu_math_stream_namespace.h>

// from sutil
#include <sutil.h>
#include <Camera.h>

#include "PpmObjLoader.h"
#include "ppm.h"
#include "random.h"

#include <imgui/imgui.h>
#include <imgui/imgui_impl_glfw.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdint.h>

using namespace optix;

const char* const SAMPLE_NAME = "optixProgressivePhotonMap";
const unsigned int WIDTH  = 768u;
const unsigned int HEIGHT = 768u;
const unsigned int MAX_PHOTON_COUNT = 2u;
const unsigned int photon_launch_dim = 512u;  // TODO
const float LIGHT_THETA = 1.15f;
const float LIGHT_PHI = 2.19f;

//------------------------------------------------------------------------------
//
// Globals
//
//------------------------------------------------------------------------------

Context      context = 0;


//------------------------------------------------------------------------------
//
//  Helper functions
//
//------------------------------------------------------------------------------
    

// Finds the smallest power of 2 greater or equal to x.
static unsigned int pow2roundup(unsigned int x)
{
  --x;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
  return x+1;
}

static int max_component(float3 a)
{
  if(a.x > a.y) {
    if(a.x > a.z) {
      return 0;
    } else {
      return 2;
    }
  } else {
    if(a.y > a.z) {
      return 1;
    } else {
      return 2;
    }
  }
}

static float3 sphericalToCartesian( float theta, float phi )
{
  float cos_theta = cosf( theta );
  float sin_theta = sinf( theta );
  float cos_phi = cosf( phi );
  float sin_phi = sinf( phi );
  float3 v;
  v.x = cos_phi * sin_theta;
  v.z = sin_phi * sin_theta;
  v.y = cos_theta;
  return v;
}

static std::string ptxPath( const std::string& cuda_file )
{
    return
        std::string(sutil::samplesPTXDir()) +
        "/" + std::string(SAMPLE_NAME) + "_generated_" +
        cuda_file +
        ".ptx";
}


static Buffer getOutputBuffer()
{
    return context[ "output_buffer" ]->getBuffer();
}


void destroyContext()
{
    if( context )
    {
        context->destroy();
        context = 0;
    }
}

enum ProgramEnum {
    rtpass,
    ppass,
    gather,
    NUM_PROGRAMS
};

void createContext( bool use_pbo )
{
    // Set up context
    context = Context::create();

    // There's a performance advantage to using a device that isn't being used as a display.
    // We'll take a guess and pick the second GPU if the second one has the same compute
    // capability as the first.
    int deviceId = 0;
    int computeCaps[2];
    if (RTresult code = rtDeviceGetAttribute(0, RT_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY, sizeof(computeCaps), &computeCaps))
        throw Exception::makeException(code, 0);
    for(unsigned int index = 1; index < Context::getDeviceCount(); ++index) {
        int computeCapsB[2];
        if (RTresult code = rtDeviceGetAttribute(index, RT_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY, sizeof(computeCaps), &computeCapsB))
            throw Exception::makeException(code, 0);
        if (computeCaps[0] == computeCapsB[0] && computeCaps[1] == computeCapsB[1]) {
            deviceId = index;
            break;
        }
    }
    context->setDevices(&deviceId, &deviceId+1);

    context->setRayTypeCount( 3 );
    context->setEntryPointCount( NUM_PROGRAMS );
    context->setStackSize( 800 );

    context["max_depth"]->setUint( 3u );
    context["max_photon_count"]->setUint( MAX_PHOTON_COUNT );

    context["scene_epsilon"]->setFloat( 1.e-1f );
    context["alpha"]->setFloat( 0.7f );
    context["total_emitted"]->setFloat( 0.0f );
    context["frame_number"]->setFloat( 0.0f );
    context["use_debug_buffer"]->setUint( 0 );  // TODO

    Buffer buffer = sutil::createOutputBuffer( context, RT_FORMAT_FLOAT4, WIDTH, HEIGHT, use_pbo );
    context["output_buffer"]->set( buffer );

    // Debug output buffer
    Buffer debug_buffer = context->createBuffer( RT_BUFFER_OUTPUT, RT_FORMAT_FLOAT4, WIDTH, HEIGHT );
    context["debug_buffer"]->set( debug_buffer );

    // RTPass output buffer
    Buffer rtpass_buffer = context->createBuffer( RT_BUFFER_OUTPUT, RT_FORMAT_USER, WIDTH, HEIGHT );
    rtpass_buffer->setElementSize( sizeof( HitRecord ) );
    context["rtpass_output_buffer"]->set( rtpass_buffer );

    // RTPass pixel sample buffers
    Buffer image_rnd_seeds = context->createBuffer( RT_BUFFER_INPUT_OUTPUT | RT_BUFFER_GPU_LOCAL, RT_FORMAT_UNSIGNED_INT2, WIDTH, HEIGHT );
    context["image_rnd_seeds"]->set( image_rnd_seeds );
    uint2* seeds = reinterpret_cast<uint2*>( image_rnd_seeds->map() );
    for ( unsigned int i = 0; i < WIDTH*HEIGHT; ++i ) {
        seeds[i] = random2u();
    }
    image_rnd_seeds->unmap();

    // RTPass ray gen program
    {
        const std::string ptx_path = ptxPath( "ppm_rtpass.cu" );
        Program ray_gen_program = context->createProgramFromPTXFile( ptx_path, "rtpass_camera" );
        context->setRayGenerationProgram( rtpass, ray_gen_program );

        // RTPass exception/miss programs
        Program exception_program = context->createProgramFromPTXFile( ptx_path, "rtpass_exception" );
        context->setExceptionProgram( rtpass, exception_program );
        context["rtpass_bad_color"]->setFloat( 0.0f, 1.0f, 0.0f );
        context->setMissProgram( rtpass, context->createProgramFromPTXFile( ptx_path, "rtpass_miss" ) );
        context["rtpass_bg_color"]->setFloat( make_float3( 0.34f, 0.55f, 0.85f ) );
    }

    // Photon pass
    const unsigned int num_photons = photon_launch_dim * photon_launch_dim * MAX_PHOTON_COUNT;
    Buffer ppass_buffer = context->createBuffer( RT_BUFFER_OUTPUT, RT_FORMAT_USER, num_photons );
    ppass_buffer->setElementSize( sizeof( PhotonRecord ) );
    context["ppass_output_buffer"]->set( ppass_buffer );

    {
        const std::string ptx_path = ptxPath( "ppm_ppass.cu");
        Program ray_gen_program = context->createProgramFromPTXFile( ptx_path, "ppass_camera" );
        context->setRayGenerationProgram( ppass, ray_gen_program );

        Buffer photon_rnd_seeds = context->createBuffer( RT_BUFFER_INPUT,
                RT_FORMAT_UNSIGNED_INT2,
                photon_launch_dim,
                photon_launch_dim );
        uint2* seeds = reinterpret_cast<uint2*>( photon_rnd_seeds->map() );
        for ( unsigned int i = 0; i < photon_launch_dim*photon_launch_dim; ++i ) {
            seeds[i] = random2u();
        }
        photon_rnd_seeds->unmap();
        context["photon_rnd_seeds"]->set( photon_rnd_seeds );
    }

    // Gather phase
    {
        const std::string ptx_path = ptxPath( "ppm_gather.cu" );
        Program gather_program = context->createProgramFromPTXFile( ptx_path, "gather" );
        context->setRayGenerationProgram( gather, gather_program );
        Program exception_program = context->createProgramFromPTXFile( ptx_path, "gather_exception" );
        context->setExceptionProgram( gather, exception_program );

        unsigned int photon_map_size = pow2roundup( num_photons ) - 1;
        Buffer photon_map = context->createBuffer( RT_BUFFER_INPUT, RT_FORMAT_USER, photon_map_size );
        photon_map->setElementSize( sizeof( PhotonRecord ) );
        context["photon_map"]->set( photon_map );
    }

}


Material createMaterial() 
{

    Program closest_hit1 = context->createProgramFromPTXFile( ptxPath( "ppm_rtpass.cu" ), "rtpass_closest_hit" );
    Program closest_hit2 = context->createProgramFromPTXFile( ptxPath( "ppm_ppass.cu" ), "ppass_closest_hit" );
    Program any_hit      = context->createProgramFromPTXFile( ptxPath( "ppm_gather.cu" ), "gather_any_hit" );
    Material material    = context->createMaterial();
    material->setClosestHitProgram( 0u, closest_hit1 );
    material->setClosestHitProgram( 1u, closest_hit2 );
    material->setAnyHitProgram( 2u, any_hit );
    return material;
}

void createGeometry( Material material )
{
    GeometryGroup geometry_group = context->createGeometryGroup();
    std::string full_path = std::string( sutil::samplesDir() ) + "/data/wedding-band.obj";

#if 0
    OptiXMesh mesh;
    mesh.context = context;
    mesh.material = material; // override with single material
    loadMesh( full_path, mesh ); 
    geometry_group->addChild( mesh.geom_instance );
    geometry_group->setAcceleration( context->createAcceleration( "Trbvh" ) );
#endif
    PpmObjLoader loader( full_path, context, geometry_group, "Trbvh" );
    loader.load();

    context["top_object"]->set( geometry_group );
    context["top_shadower"]->set( geometry_group );
}

void createLight( PPMLight& light )
{
    light.is_area_light = 0; 
    light.position  = 1000.0f * sphericalToCartesian( LIGHT_THETA, LIGHT_PHI );
    light.direction = normalize( make_float3( 0.0f, 0.0f, 0.0f )  - light.position );
    light.radius    = 5.0f *0.01745329252f;
    light.power     = make_float3( 0.5e4f, 0.5e4f, 0.5e4f );
    context["light"]->setUserData( sizeof(PPMLight), &light );
    context["rtpass_default_radius2"]->setFloat( 0.25f);
    context["ambient_light"]->setFloat( 0.1f, 0.1f, 0.1f);
    const std::string full_path = std::string( sutil::samplesDir() ) + "/data/CedarCity.hdr";
    const float3 default_color = make_float3( 0.8f, 0.88f, 0.97f );
    context["envmap"]->setTextureSampler( sutil::loadTexture( context, full_path, default_color) );
}




//------------------------------------------------------------------------------
//
//  GLFW callbacks
//
//------------------------------------------------------------------------------

struct CallbackData
{
    sutil::Camera& camera;
    unsigned int& accumulation_frame;
};

void keyCallback( GLFWwindow* window, int key, int scancode, int action, int mods )
{
    bool handled = false;

    if( action == GLFW_PRESS )
    {
        switch( key )
        {
            case GLFW_KEY_Q:
            case GLFW_KEY_ESCAPE:
                if( context )
                    context->destroy();
                if( window )
                    glfwDestroyWindow( window );
                glfwTerminate();
                exit(EXIT_SUCCESS);

            case( GLFW_KEY_S ):
            {
                const std::string outputImage = std::string(SAMPLE_NAME) + ".png";
                std::cerr << "Saving current frame to '" << outputImage << "'\n";
                sutil::writeBufferToFile( outputImage.c_str(), getOutputBuffer() );
                handled = true;
                break;
            }
            case( GLFW_KEY_F ):
            {
               CallbackData* cb = static_cast<CallbackData*>( glfwGetWindowUserPointer( window ) );
               cb->camera.reset_lookat();
               cb->accumulation_frame = 0;
               handled = true;
               break;
            }
        }
    }

    if (!handled) {
        // forward key event to imgui
        ImGui_ImplGlfw_KeyCallback( window, key, scancode, action, mods );
    }
}

void windowSizeCallback( GLFWwindow* window, int w, int h )
{
    if (w < 0 || h < 0) return;

    const unsigned width = (unsigned)w;
    const unsigned height = (unsigned)h;

    CallbackData* cb = static_cast<CallbackData*>( glfwGetWindowUserPointer( window ) );
    if ( cb->camera.resize( width, height ) ) {
        cb->accumulation_frame = 0;
    }

    sutil::resizeBuffer( getOutputBuffer(), width, height );

    // TODO: not by name
    sutil::resizeBuffer( context[ "debug_buffer" ]->getBuffer(), width, height );
    sutil::resizeBuffer( context[ "rtpass_output_buffer" ]->getBuffer(), width, height );
    sutil::resizeBuffer( context[ "image_rnd_seeds" ]->getBuffer(), width, height );

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1, 0, 1, -1, 1);
    glViewport(0, 0, width, height);
}


//------------------------------------------------------------------------------
//
// GLFW setup and run 
//
//------------------------------------------------------------------------------

GLFWwindow* glfwInitialize( )
{
    GLFWwindow* window = sutil::initGLFW();

    // Note: this overrides imgui key callback with our own.  We'll chain this.
    glfwSetKeyCallback( window, keyCallback );

    glfwSetWindowSize( window, (int)WIDTH, (int)HEIGHT );
    glfwSetWindowSizeCallback( window, windowSizeCallback );

    return window;
}


void glfwRun( GLFWwindow* window, sutil::Camera& camera )
{
    // Initialize GL state
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1, 0, 1, -1, 1 );
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glViewport(0, 0, WIDTH, HEIGHT );

    unsigned int frame_count = 0;
    unsigned int accumulation_frame = 0;
    int max_depth = 10;
    bool draw_ground = true;

    // Expose user data for access in GLFW callback functions when the window is resized, etc.
    // This avoids having to make it global.
    CallbackData cb = { camera, accumulation_frame };
    glfwSetWindowUserPointer( window, &cb );

    while( !glfwWindowShouldClose( window ) )
    {

        glfwPollEvents();                                                        

        ImGui_ImplGlfw_NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        
        // Let imgui process the mouse first
        if (!io.WantCaptureMouse) {

            double x, y;
            glfwGetCursorPos( window, &x, &y );

            if ( camera.process_mouse( (float)x, (float)y, ImGui::IsMouseDown(0), ImGui::IsMouseDown(1), ImGui::IsMouseDown(2) ) ) {
                accumulation_frame = 0; 
            }
        }

        // imgui pushes
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,   ImVec2(0,0) );
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,          0.6f        );
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0f        );

        sutil::displayFps( frame_count++ );

        // imgui pops
        ImGui::PopStyleVar( 3 );

        // Render main window

        context["frame_number"]->setFloat( static_cast<float>( accumulation_frame++ ) );
        if ( accumulation_frame == 1 ) {
            // Trace viewing rays
            context->launch( rtpass, camera.width(), camera.height() );
            context["total_emitted"]->setFloat(  0.0f );
        }

        // Trace photons
        {
            Buffer photon_rnd_seeds = context["photon_rnd_seeds"]->getBuffer();
            uint2* seeds = reinterpret_cast<uint2*>( photon_rnd_seeds->map() );
            for ( unsigned int i = 0; i < photon_launch_dim*photon_launch_dim; ++i ) {
                seeds[i] = random2u();
            }
            photon_rnd_seeds->unmap();
            context->launch( ppass, photon_launch_dim, photon_launch_dim );
        }

        // By computing the total number of photons as an unsigned long long we avoid 32 bit
        // floating point addition errors when the number of photons gets sufficiently large
        // (the error of adding two floating point numbers when the mantissa bits no longer
        // overlap).
        context["total_emitted"]->setFloat( static_cast<float>((unsigned long long)accumulation_frame*photon_launch_dim*photon_launch_dim) );

        // Build KD tree   TODO
        //createPhotonMap();

        // Shade view rays by gathering photons
        context->launch( gather, camera.width(), camera.height() );
        sutil::displayBufferGL( getOutputBuffer() );

        // TODO: debug output

        // Render gui over it
        ImGui::Render();

        glfwSwapBuffers( window );
    }
    
    destroyContext();
    glfwDestroyWindow( window );
    glfwTerminate();
}


//------------------------------------------------------------------------------
//
// Main
//
//------------------------------------------------------------------------------

void printUsageAndExit( const std::string& argv0 )
{
    std::cerr << "\nUsage: " << argv0 << " [options]\n";
    std::cerr <<
        "App Options:\n"
        "  -h | --help                  Print this usage message and exit.\n"
        "  -f | --file <output_file>    Save image to file and exit.\n"
        "  -n | --nopbo                 Disable GL interop for display buffer.\n"
        "App Keystrokes:\n"
        "  q  Quit\n"
        "  s  Save image to '" << SAMPLE_NAME << ".png'\n"
        "  f  Re-center camera\n"
        "\n"
        << std::endl;

    exit(1);
}


int main( int argc, char** argv )
{
    bool use_pbo = true;
    std::string out_file;
    std::vector<std::string> mesh_files;
    std::vector<optix::Matrix4x4> mesh_xforms;
    for( int i=1; i<argc; ++i )
    {
        const std::string arg( argv[i] );

        if( arg == "-h" || arg == "--help" )
        {
            printUsageAndExit( argv[0] );
        }
        else if( arg == "-f" || arg == "--file"  )
        {
            if( i == argc-1 )
            {
                std::cerr << "Option '" << arg << "' requires additional argument.\n";
                printUsageAndExit( argv[0] );
            }
            out_file = argv[++i];
        }
        else if( arg == "-n" || arg == "--nopbo"  )
        {
            use_pbo = false;
        }
        else if( arg[0] == '-' )
        {
            std::cerr << "Unknown option '" << arg << "'\n";
            printUsageAndExit( argv[0] );
        }
        else {
            // Interpret argument as a mesh file.
            mesh_files.push_back( argv[i] );
            mesh_xforms.push_back( optix::Matrix4x4::identity() );
        }
    }

    try
    {
        GLFWwindow* window = glfwInitialize();

#ifndef __APPLE__
        GLenum err = glewInit();
        if (err != GLEW_OK)
        {
            std::cerr << "GLEW init failed: " << glewGetErrorString( err ) << std::endl;
            exit(EXIT_FAILURE);
        }
#endif

        createContext( use_pbo );

        // initial camera data
        const optix::float3 camera_eye( optix::make_float3( -235.0f, 220.0f, 0.0f ) );
        const optix::float3 camera_lookat( optix::make_float3( 0.0f, 0.0f, 0.0f ) );
        const optix::float3 camera_up( optix::make_float3( 0.0f, 1.0f, 0.0f ) );
        sutil::Camera camera( WIDTH, HEIGHT, 
                &camera_eye.x, &camera_lookat.x, &camera_up.x,
                context["rtpass_eye"], context["rtpass_U"], context["rtpass_V"], context["rtpass_W"] );

        Material material = createMaterial();
        createGeometry( material );
        PPMLight light;
        createLight( light );

        context->validate();
        
        if ( out_file.empty() )
        {
            glfwRun( window, camera );
        }
        else
        {
           // TODO: batch mode 
        }
        return 0;
    }
    SUTIL_CATCH( context->get() )
}


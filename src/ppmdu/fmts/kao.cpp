#include "kao.hpp"
#include <ppmdu/utils/utility.hpp>
#include <ppmdu/ext_fmts/png_io.hpp>
#include <ppmdu/ext_fmts/bmp_io.hpp>
#include <ppmdu/ext_fmts/rawimg_io.hpp>
#include <ppmdu/ext_fmts/riff_palette.hpp>
#include <iostream>
#include <cassert>
#include <map>
#include <sstream>
#include <utility>
#include <iomanip>
#include <ppmdu/fmts/at4px.hpp>
#include <Poco/DirectoryIterator.h>
#include <Poco/File.h>
#include <Poco/Path.h>
#include <ppmdu/utils/gbyteutils.hpp>
using namespace std;
using namespace gimg;

namespace pmd2 { namespace filetypes 
{

//========================================================================================================
//  Typedefs
//========================================================================================================
    //typedef to make things readable
    typedef map<uint32_t, types::bytevec_szty_t>::const_iterator cmapiter_t;

//========================================================================================================
//  Utility Functions
//========================================================================================================

    inline bool IsValidFile( const Poco::File & f ) //A little function for determining valid files
    { 
        return f.isFile() && !(f.isHidden()); 
    }

    inline bool IsValidDirectory( const Poco::File & f ) //A little function for determining valid files
    { 
        return f.isDirectory() && !(f.isHidden()); 
    }

//==================================================================
// Structs
//==================================================================

    //This is to avoid including the POCO header in the "kao.hpp" file..
    struct kao_file_wrapper
    {
        Poco::File myfile;
    };

//========================================================================================================
//  CKaomado
//========================================================================================================
    const array<CKaomado::fexthndlr_t, 3> CKaomado::SupportedInputImageTypes =
    {{
        { pngio::PNG_FileExtension,        eEXPORT_t::EX_PNG },
        { bmpio::BMP_FileExtension,        eEXPORT_t::EX_BMP },
        { rawimg_io::RawImg_FileExtension, eEXPORT_t::EX_RAW },
    }};


    CKaomado::CKaomado( unsigned int nbentries, unsigned int nbsubentries )
        :m_nbtocsubentries(nbsubentries), m_tableofcontent(nbentries)/*,
        m_imgdata( nbentries * nbsubentries) */ //This pre-alloc caused the whole thing to hang for several seconds when constructing
    {
        m_tableofcontent.resize(0); //reset the size to 0, without de-allocating
        m_imgdata.resize(0);
    }

    bool CKaomado::ReadEntireKaomado( types::constitbyte_t itinbegin, types::constitbyte_t itinend, bool bBeQuiet )
    {
        //#1 - Iterate the ToC
        //Get the first null entry out of the way first to avoid checking each turns..
        auto    itfoundnonnull = std::find_if_not( itinbegin, itinend, [](const uint8_t& val){ return val == 0; } );
        int32_t firstentrylen  = std::distance( itinbegin, itfoundnonnull );

        //Entry size check 
        if( ( firstentrylen / SUBENTRY_SIZE ) > m_nbtocsubentries )
        {
            assert(false); //IMPLEMENT VARIABLE SIZE ENTRIES IF EVEN POSSIBLE!
            throw std::length_error("Error: First null entry has unexpected length of " + std::to_string(firstentrylen) );
            return false;
        }
        //If the entire thing is null..
        if( itfoundnonnull == itinend )
        {
            assert(false);
            throw std::runtime_error("Error: Entire kaomado is null!");
            return false;
        }

        //When we got our first non-null entry, and divide the ptr by the size of an entry, to get the amount of entries
        // And avoid further resizing.
        //Get the first non-null entry and get the nb of entries in the table of content of the file being read
        tocsubentry_t   firsptr      = utils::ReadIntFromByteVector<tocsubentry_t>(itfoundnonnull);
        uint32_t        nbtocentries = firsptr / (m_nbtocsubentries * SUBENTRY_SIZE);
        auto            ittoc        = itinbegin;
        vector<uint8_t> imagebuffer;    //A temporary buffer that images uses to decompress to. 
                                        //Its here to avoid having to re-allocate a new one for each toc sub-entries!

        //Ensure capacity
        m_tableofcontent.resize(nbtocentries, kao_toc_entry(m_nbtocsubentries) );
        m_imgdata       .resize(nbtocentries * m_nbtocsubentries); //Reserve extra slots to avoid having to shrink/grow when modifying
                                                                   // and ensuring easier/faster rebuilding of the ToC and file, by
                                                                   // avoiding having to update all references in the ToC.

        if(!bBeQuiet)
        {
            cout<<"Parsing kaomado file..\n";
            for( std::size_t i = 0; i < m_tableofcontent.size(); )
            {
                ittoc = ReadAToCEntry( i, itinbegin, ittoc, imagebuffer, bBeQuiet );
                ++i;
                cout<<"\r" << ((i * 100) / m_tableofcontent.size())  <<"%";
            }
            cout << "\n";
        }
        else
        {
            for( std::size_t i = 0; i < m_tableofcontent.size(); ++i )
                ittoc = ReadAToCEntry( i, itinbegin, ittoc, imagebuffer, bBeQuiet );
        }

        return true;
    }

    //itdatabeg
    uint32_t CKaomado::CalculateEntryLen( types::constitbyte_t itdatabeg, tocsubentry_t entryoffset )
    {
        //Skip palette, and read at4px header
        at4px_header head;
        std::advance( itdatabeg, 
                      static_cast<decltype(KAO_PORTRAIT_PAL_LEN)>(entryoffset) + KAO_PORTRAIT_PAL_LEN ); 
        head.ReadFromContainer( itdatabeg );
        return head.compressedsz + KAO_PORTRAIT_PAL_LEN;
    }

    CKaomado::tocsubentry_t CKaomado::ReadAToCSubEntry( types::constitbyte_t & itbegindata )
    {
        return utils::ReadIntFromByteVector<tocsubentry_t>( itbegindata );
    }

    bool CKaomado::isToCSubEntryValid( const tocsubentry_t & entry )const
    {
        return (entry > 0);
    }

    CKaomado::tocsubentry_t CKaomado::GetInvalidToCEntry()
    {
        return numeric_limits<tocsubentry_t>::min();
    }

    types::constitbyte_t CKaomado::ReadAToCEntry( vector<kao_toc_entry>::size_type  & indexentry,
                                                  types::constitbyte_t                itbegindata, 
                                                  types::constitbyte_t                itrawtocentry, 
                                                  std::vector<uint8_t>  &            imagebuffer,
                                                  bool                               bBeQuiet ) 
    {
        //Alias to make things a little more readable
        vector<tocsubentry_t> & currententry = m_tableofcontent[indexentry]._portraitsentries;

        //Go through all our ToC entry's sub-entries
        for( tocsz_t cptsubentry = 0; cptsubentry <  currententry.size(); ++cptsubentry )
        {
            tocsubentry_t tocreadentry; //The entry we just read in the last loop
            tocreadentry = ReadAToCSubEntry( itrawtocentry );
            
            //Avoid null and invalid entries
            if( isToCSubEntryValid(tocreadentry) )
            {
                uint32_t entrylen       = CalculateEntryLen( itbegindata, tocreadentry );
                tocsz_t  entryinsertpos = (indexentry * DEF_KAO_TOC_ENTRY_NB_PTR) + cptsubentry; //Position to insert stuff for this entry in the data vector
                data_t & tmpPortrait    = m_imgdata[entryinsertpos]; //a little reference to make things easier
                auto     itentryread    = itbegindata + tocreadentry;
                auto     itentryend     = itbegindata + (tocreadentry + entrylen);
                auto     itpalend       = itbegindata + (tocreadentry + KAO_PORTRAIT_PAL_NB_COL * KAO_PORTRAIT_PAL_BPC);

                //A. Read the palette
                graphics::ReadRawPalette_RGB24_As_RGB24( itentryread, itpalend, tmpPortrait.getPalette() );

                //B. Read the image
                filetypes::DecompressAT4PX( itpalend, itentryend, imagebuffer );

                //C. Parse the image. 
                // Image pixels seems to be in little endian, and need to be converted to big endian
                ParseTiledImg<data_t>( imagebuffer.begin(), 
                                       imagebuffer.end(), 
                                       graphics::RES_PORTRAIT, 
                                       tmpPortrait, 
                                       KAO_PORTRAIT_PIXEL_ORDER_REVERSED );

                //Refer to the new entry
                registerToCEntry( indexentry, cptsubentry, entryinsertpos );
            }
            else
                currententry[cptsubentry] = GetInvalidToCEntry(); //Set anything invalid to invalid!
        }
        return itrawtocentry;
    }

    pair<uint32_t,uint32_t> CKaomado::EstimateToCLengthAndBiggestImage()const
    {
        //Estimate kaomado worst case scenario length. (The size as if all images weren't compressed)
        unsigned int estimatedlength = m_tableofcontent.size() * (m_nbtocsubentries * SUBENTRY_SIZE);
        unsigned int szBiggestImg    = 0;

        //Use the ToC to count only valid entries!
        for( auto & entry : m_tableofcontent )
        {
            for( auto & subentry : entry._portraitsentries )
            {
                if( isToCSubEntryValid( subentry ) )
                {
                    unsigned int  currentimgsz = (m_imgdata[subentry].getSizeInBits() / 8u) + 
                                                 KAO_PORTRAIT_PAL_LEN + 
                                                 at4px_header::HEADER_SZ;

                    if( szBiggestImg < currentimgsz ) //Keep track of the largest image, so we can avoid resizing our compression buffer later on
                        szBiggestImg = (currentimgsz / 8u) + currentimgsz; //Reserve some extra space for command bytes just in case

                    estimatedlength += (currentimgsz / 8u) + currentimgsz; //Reserve some extra space for command bytes just in case
                }
            }
        }
        return make_pair(estimatedlength, szBiggestImg);
    }

    void CKaomado::WriteAPortrait( kao_toc_entry::subentry_t &                     portrait,
                                   std::vector<uint8_t> &                          outputbuffer, 
                                   std::back_insert_iterator<std::vector<uint8_t>> itoutputpushback,
                                   std::vector<uint8_t> &                          imgbuffer, 
                                   std::back_insert_iterator<std::vector<uint8_t>> itimgbufpushback,
                                   bool                                            bZealousStringSearchEnabled,
                                   tocsubentry_t &                                 lastvalideendofdataoffset,
                                   uint32_t &                                      offsetWriteatTocSub )
    {
        //First set both to the last valid end of data offset. "null" them out basically!
        tocsubentry_t portraitpointer    = lastvalideendofdataoffset; //The offset from the beginning where we'll insert any new data!

        //Make sure we output a computed file offset for valid entry, and just the entry's value if the pointer value is invalid
        if( isToCSubEntryValid( portrait ) )
        {
            //If we have data to write
            auto & currentimg = m_imgdata[portrait];
            portraitpointer   = outputbuffer.size(); //Set the current size as the offset to insert our stuff!

            //#3.1 - Write palette
            graphics::WriteRawPalette_RGB24_As_RGB24( itoutputpushback, currentimg.getPalette().begin(), currentimg.getPalette().end() );

            //Keep track of where the at4px begins
            auto offsetafterpal = outputbuffer.size();

            //#3.2 - Copy the image into something raw
            unsigned int curimgmaxsz      = (currentimg.getSizeInBits() / 8u) +
                                            ( (currentimg.getSizeInBits() % 8u != 0)? 1u : 0u); //Add one more just in case we ever overflow a byte (fat chance..)
            unsigned int curimgszandheadr = curimgmaxsz + at4px_header::HEADER_SZ;

            // Make a raw tiled image
            imgbuffer.resize(0);
            WriteTiledImg( itimgbufpushback, currentimg, KAO_PORTRAIT_PIXEL_ORDER_REVERSED );

            //#3.3 - Expand the output to at least the raw image's length, then write at4px
            compression::px_info_header pxinf = CompressToAT4PX( imgbuffer.begin(), 
                                                                    imgbuffer.end(), 
                                                                    itoutputpushback,
                                                                    compression::ePXCompLevel::LEVEL_3,
                                                                    bZealousStringSearchEnabled );

            //Update the last valid end of data offset (We fill any subsequent invalid entry with this value!)
            lastvalideendofdataoffset = - (static_cast<tocsubentry_t>(outputbuffer.size())); //Change the sign to negative too
        }

        //Write the ToC entry in the space we reserved at the beginning of the output buffer!
        utils::WriteIntToByteVector( portraitpointer,    outputbuffer.begin() + offsetWriteatTocSub );
        offsetWriteatTocSub += sizeof(portraitpointer);
    }

    std::vector<uint8_t> CKaomado::WriteKaomado(bool bBeQuiet, bool bZealousStringSearch )
    {
        if( m_imgdata.empty() || m_tableofcontent.empty() )
        {
            cerr << "<!>-WARNING: CKaomado::WriteKaomado() : Nothing to write in the output kaomado file!\n";
            assert(false);
            return vector<uint8_t>();
        }

        //#0 - Gather some stats and do some checks
        const auto EXPECTED_TOC_LENGTH = m_tableofcontent.size() * (m_nbtocsubentries * SUBENTRY_SIZE);

        //#1 - Estimate kaomado worst case scenario length. (The size as if all images weren't compressed)
        auto                 resultlenghts      = EstimateToCLengthAndBiggestImage();
        unsigned int         estimatedlength    = resultlenghts.first;
        unsigned int         szBiggestImg       = resultlenghts.second;
        std::vector<uint8_t> imgbuffer( szBiggestImg, 0 );
        std::vector<uint8_t> outputbuffer( utils::GetNextInt32DivisibleBy16( estimatedlength ), 0 ); //align on 16 bytes

        unsigned int         curoffsetToc       = 0;
        unsigned int         offsetTocEnd       = EXPECTED_TOC_LENGTH;
        unsigned int         cptcompletion      = 0;
        unsigned int         nbentries          = m_tableofcontent.size();
        tocsubentry_t        lastValidEoDOffset = 0; //Used for filling in null entries! All null entries refer to the last valid end of data offset!
        auto                 itimgbufpushback   = std::back_inserter(imgbuffer);
        auto                 itoutputpushback   = std::back_inserter(outputbuffer);

        //Resize to ToC lenght so can begin inserting afterwards
        outputbuffer.resize(EXPECTED_TOC_LENGTH); 

        //#2 - Skip the ToC in the output, and begin outputing portraits, writing down their offset as we go.
        if( !bBeQuiet )
            cout << "Building kaomado file..\n";

        for( auto& tocentry : m_tableofcontent )
        {
            uint32_t offsetWriteatTocSub = curoffsetToc; //Where we'll be writing our next sub-entrie

            for( auto & portrait : tocentry._portraitsentries )
            {
                WriteAPortrait( portrait, 
                                outputbuffer, 
                                itoutputpushback, 
                                imgbuffer, 
                                itimgbufpushback, 
                                bZealousStringSearch, 
                                lastValidEoDOffset, 
                                offsetWriteatTocSub );
            }
            curoffsetToc += (m_nbtocsubentries * SUBENTRY_SIZE); //jump to next ToC entry

            ++cptcompletion;
            if( !bBeQuiet )
                cout<<"\r" << (cptcompletion * 100) / nbentries <<"%";
        }
        if( !bBeQuiet )
            cout<<"\n";

        //Align the end of the file on 16 bytes
        unsigned int nbpaddingbytes = utils::GetNextInt32DivisibleBy16( outputbuffer.size() ) - outputbuffer.size();

        for( int i = 0; i < nbpaddingbytes; ++i )
            outputbuffer.push_back(COMMON_PADDING_BYTE);

        //Done, move the vector
        return std::move( outputbuffer );
    }

    bool CKaomado::isSupportedImageType( const std::string & path )const
    {
        Poco::Path imagepath( path );
        string     imgext     = imagepath.getExtension();

        for( auto & afiletype : SupportedInputImageTypes )
        {
            if( imgext.compare( afiletype.extension ) == 0 )
                return true;
        }
        return false;
    }

    //Returns index inserted at!
    vector<kao_toc_entry>::size_type CKaomado::InputAnImageToDataVector( kao_file_wrapper & imagetohandle )
    {
        data_t     palimg;
        Poco::Path imagepath( imagetohandle.myfile.path() );
        string     imgext     = imagepath.getExtension();
        eEXPORT_t  detectedty = eEXPORT_t::EX_INVALID;

        for( auto & afiletype : SupportedInputImageTypes )
        {
            if( imgext.compare( afiletype.extension ) == 0 )
            {
                detectedty = afiletype.detectedtype;
                break;
            }
        }

        //Proceed to validate the file and find out what to use to handle it!
        switch( detectedty )
        {
            case eEXPORT_t::EX_PNG:
            {
                pngio::ImportFrom4bppPNG( palimg, imagetohandle.myfile.path() );
                break;
            }
            case eEXPORT_t::EX_BMP:
            {
                bmpio::ImportFrom4bppBMP( palimg, imagetohandle.myfile.path() );
                break;
            }
            case eEXPORT_t::EX_RAW:
            {
                stringstream pathtoraw;
                pathtoraw << imagepath.parent().toString() << imagepath.getBaseName();
                rawimg_io::ImportFrom4bppRawImgAndPal( palimg, pathtoraw.str(), graphics::RES_PORTRAIT );
                break;
            }
            default:
            {
                stringstream strserror;
                strserror<< "<!>-Error: Image " <<imagepath.toString() <<" doesn't look like a BMP, RAW or PNG image !";
                throw std::runtime_error(strserror.str());
            }
        };

        m_imgdata.push_back( std::move(palimg) );
        return m_imgdata.size() - 1;
    }

    void CKaomado::HandleAFolder( kao_file_wrapper & foldertohandle )
    {
        Poco::File              & thefolder = foldertohandle.myfile;
        Poco::DirectoryIterator   itdir(thefolder),
                                  itdirend;
        string                    foldername = Poco::Path(thefolder.path()).makeFile().getBaseName();
        vector<Poco::File>        validImages(DEF_KAO_TOC_ENTRY_NB_PTR);
        validImages.resize(0);

        //#1 - Find all our valid images
        for(; itdir != itdirend; ++itdir ) 
        {
            //if( IsValidFile(*itdir) )
            if( isSupportedImageType(itdir->path()) )
                validImages.push_back(*itdir);
        }

        //#2 - Output diagnosis to console about what files are valid and not

        //Use folder names as indexes in the kaomado table, use image file names as toc subentry indexes!
        stringstream strsfoldername;
        unsigned int ToCindex = 0;
        strsfoldername << foldername;
        strsfoldername >> ToCindex; //Parse the ToC index from the beginning of the foldername

        if( ToCindex >= m_tableofcontent.size() )
        {
            cerr <<"The index number in the name of folder " <<foldername <<", is higher than the number of slots available in the ToC!\n"
                 <<"It will be ignored !. Next time, please number your folders from 1 to " <<(m_tableofcontent.size()-1) <<"!\n";
            return;
        }

        //#3 - Convert the PNGs to tiled 4bpp images and color palettes
        for( auto & animage : validImages )
        {
            kao_file_wrapper wrap;
            wrap.myfile = animage;
            unsigned int datavecindex = 0;

            try
            {
                datavecindex = InputAnImageToDataVector(wrap); //Add image to data vector
            }
            catch(exception e)
            {
                cerr <<"\n" <<e.what() <<"\n";
                continue; //skip the rest in case we had issues with this image!
            }

            //Parse index from image name
            Poco::Path   imgpath(animage.path());
            stringstream sstrimgindex;
            unsigned int imgindex = 0;

            sstrimgindex << imgpath.getBaseName();
            sstrimgindex >> imgindex;

            //validate
            if( imgindex >= m_tableofcontent.front()._portraitsentries.size() )
            {
                stringstream strserror;
                strserror <<"The index of " <<imgpath.getFileName() <<", in folder name " <<foldername 
                          <<", exceeds the number of slots available, " <<(m_nbtocsubentries-1) 
                          <<" !\n"
                          <<"Please, number the images from 0 to " <<(m_nbtocsubentries-1) <<"!\n";
                assert(false);
                throw domain_error( strserror.str() );
            }

            registerToCEntry( ToCindex, imgindex, datavecindex );
        }
    }

    void CKaomado::BuildFromFolder( std::string & folderpath, bool bBeQuiet )
    {
        //Index name starts at 1 given index 0 holds the dummy first entry
        Poco::DirectoryIterator itdir(folderpath),
                                itdirend;
        vector<Poco::File>      ValidDirectories( m_tableofcontent.capacity() );
        //Resize to zero for pushback and preserve alloc
        ValidDirectories.resize(0);

        //Resize the ToC to its set size ! And set all entries to null by default !
        m_tableofcontent.resize( m_tableofcontent.capacity(), kao_toc_entry(DEF_KAO_TOC_ENTRY_NB_PTR) );

        //#1 - Count nb folders
        for( ; itdir != itdirend; ++itdir)
        {
            if( IsValidDirectory(*itdir) ) 
                ValidDirectories.push_back( *itdir );
        }

        if( ValidDirectories.size() == 0 )
            cout <<"<!>-Warning: Folder to build Kaomado from contain no valid directories!\n";
        else if( !bBeQuiet )
            cout <<"Found " <<ValidDirectories.size() <<" valid directories!\n";

        m_imgdata.reserve( (ValidDirectories.size() * m_nbtocsubentries) + 1u );  //Reserve some space +1 for the dummy first slot !
        m_imgdata.resize(1u); //Skip the first entry, so the first data entry in the toc is not null ! 

        if(!bBeQuiet)
            cout<<"Reading Sub-Directories..\n";

        //#2 - Handle all folders
        unsigned int cptdirs = 0;
        for( Poco::DirectoryIterator ithandledir(folderpath); ithandledir != itdirend; ++ithandledir )
        {
            //Need to wrap it so we keep the header clean.. It will probably get optimized out anyways by the compiler
            kao_file_wrapper wrap;
            wrap.myfile = *ithandledir;
            HandleAFolder( wrap );

            ++cptdirs;
            if(!bBeQuiet)
                cout<<"\r" <<(cptdirs*100) / ValidDirectories.size() <<"%";
        }
        if( !bBeQuiet )
            cout<<"\n";
    }


    void CKaomado::ExportToFolders( std::string          & folderpath, 
                                    const vector<string> * pfoldernames, 
                                    const vector<string> * psubentrynames,
                                    eEXPORT_t              exporttype,
                                    bool                   bBeQuiet )
    {
        //#1 - Go through the ToC, and make a sub-folder for each ToC entry
        //     with its index as name.

        //Make the parent folder
        utils::DoCreateDirectory( folderpath );
        
        if( !bBeQuiet )
            cout<<"Exporting entries to folder..\n";

        for( tocsz_t i = 1; i < m_tableofcontent.size(); )
        {
            //Create the sub-folder
            stringstream outfoldernamess;
            outfoldernamess <<utils::AppendTraillingSlashIfNotThere(folderpath)
                             <<std::setfill('0') <<std::setw(4) <<std::dec <<i;

            if( pfoldernames != nullptr && pfoldernames->size() > i && !pfoldernames->at(i).empty() )
            {
                outfoldernamess<< "_" << pfoldernames->at(i);
            }

            //Make an alias and exporter
            string                     outfoldername = outfoldernamess.str();
            vector<tocsubentry_t>    & curtocentry   = m_tableofcontent[i]._portraitsentries;

            ExportAToCEntry( m_tableofcontent[i]._portraitsentries, outfoldername, psubentrynames, exporttype );

            ++i;
            if( !bBeQuiet )
                cout<<"\r" << (i*100) / m_tableofcontent.size() <<"%";
        }
        if( !bBeQuiet )
            cout<<"\n";
    }

    void CKaomado::ExportAToCEntry( const vector<tocsubentry_t> & entry, 
                                    const string                & outputfoldername, 
                                    const vector<string>        * psubentrynames,
                                    eEXPORT_t                     exporttype )
    {
        bool bmadeafolder = false; //Whether we made a folder already for this entry.
                                   // We're using it this way, because we don't want to create empty folders!

        for( tocsz_t j = 0; j < entry.size(); ++j )
        {
            if( isToCSubEntryValid( entry[j] ) ) //If not a dummy value
            {
                if(!bmadeafolder)
                {
                    utils::DoCreateDirectory(outputfoldername);
                    bmadeafolder = true;
                }

                string       suffix = (psubentrynames!=nullptr && psubentrynames->size() > j )? (*psubentrynames)[j] : string();
                stringstream ss;
                ss << utils::AppendTraillingSlashIfNotThere(outputfoldername) << setfill('0') << setw(4)  << static_cast<uint32_t>(j); 

                if( !suffix.empty() )
                    ss <<"_" <<suffix;

                if( exporttype == eEXPORT_t::EX_RAW )
                {
                    //Don't append anything! We need only the filename, no extension!
                    rawimg_io::ExportTo4bppRawImgAndPal( m_imgdata[entry[j]], ss.str() );
                }
                else if( exporttype == eEXPORT_t::EX_BMP )
                {
                    ss <<"." << bmpio::BMP_FileExtension;
                    bmpio::ExportTo4bppBMP( m_imgdata[entry[j]], ss.str() );
                }
                else
                {
                    //If all fail, export to PNG !
                    ss <<"." << pngio::PNG_FileExtension;
                    pngio::ExportTo4bppPNG( m_imgdata[entry[j]], ss.str() );
                }
            }
        }
    }

};};